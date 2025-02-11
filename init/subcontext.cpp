/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "subcontext.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <selinux/android.h>

#include "action.h"
#include "builtins.h"
#include "proto_utils.h"
#include "util.h"

#if defined(__ANDROID__)
#include <android/api-level.h>
#include "property_service.h"
#include "selabel.h"
#include "selinux.h"
#else
#include "host_init_stubs.h"
#endif

using android::base::GetExecutablePath;
using android::base::Join;
using android::base::Socketpair;
using android::base::Split;
using android::base::StartsWith;
using android::base::unique_fd;

namespace android {
namespace init {
namespace {

class SubcontextProcess {
  public:
    SubcontextProcess(const BuiltinFunctionMap* function_map, std::string context, int init_fd)
        : function_map_(function_map), context_(std::move(context)), init_fd_(init_fd){};
    void MainLoop();

  private:
    void RunCommand(const SubcontextCommand::ExecuteCommand& execute_command,
                    SubcontextReply* reply) const;
    void ExpandArgs(const SubcontextCommand::ExpandArgsCommand& expand_args_command,
                    SubcontextReply* reply) const;

    const BuiltinFunctionMap* function_map_;
    const std::string context_;
    const int init_fd_;
};

void SubcontextProcess::RunCommand(const SubcontextCommand::ExecuteCommand& execute_command,
                                   SubcontextReply* reply) const {
    // Need to use ArraySplice instead of this code.
    auto args = std::vector<std::string>();
    for (const auto& string : execute_command.args()) {
        args.emplace_back(string);
    }

    auto map_result = function_map_->Find(args);
    Result<void> result;
    if (!map_result) {
        result = Error() << "Cannot find command: " << map_result.error();
    } else {
        result = RunBuiltinFunction(map_result->function, args, context_);
    }

    if (result) {
        reply->set_success(true);
    } else {
        auto* failure = reply->mutable_failure();
        failure->set_error_string(result.error().message());
        failure->set_error_errno(result.error().code());
    }
}

void SubcontextProcess::ExpandArgs(const SubcontextCommand::ExpandArgsCommand& expand_args_command,
                                   SubcontextReply* reply) const {
    for (const auto& arg : expand_args_command.args()) {
        auto expanded_arg = ExpandProps(arg);
        if (!expanded_arg) {
            auto* failure = reply->mutable_failure();
            failure->set_error_string(expanded_arg.error().message());
            failure->set_error_errno(0);
            return;
        } else {
            auto* expand_args_reply = reply->mutable_expand_args_reply();
            expand_args_reply->add_expanded_args(*expanded_arg);
        }
    }
}

void SubcontextProcess::MainLoop() {
    pollfd ufd[1];
    ufd[0].events = POLLIN;
    ufd[0].fd = init_fd_;

    while (true) {
        ufd[0].revents = 0;
        int nr = TEMP_FAILURE_RETRY(poll(ufd, arraysize(ufd), -1));
        if (nr == 0) continue;
        if (nr < 0) {
            PLOG(FATAL) << "poll() of subcontext socket failed, continuing";
        }

        auto init_message = ReadMessage(init_fd_);
        if (!init_message) {
            if (init_message.error().code() == 0) {
                // If the init file descriptor was closed, let's exit quietly. If
                // this was accidental, init will restart us. If init died, this
                // avoids calling abort(3) unnecessarily.
                return;
            }
            LOG(FATAL) << "Could not read message from init: " << init_message.error();
        }

        auto subcontext_command = SubcontextCommand();
        if (!subcontext_command.ParseFromString(*init_message)) {
            LOG(FATAL) << "Unable to parse message from init";
        }

        auto reply = SubcontextReply();
        switch (subcontext_command.command_case()) {
            case SubcontextCommand::kExecuteCommand: {
                RunCommand(subcontext_command.execute_command(), &reply);
                break;
            }
            case SubcontextCommand::kExpandArgsCommand: {
                ExpandArgs(subcontext_command.expand_args_command(), &reply);
                break;
            }
            default:
                LOG(FATAL) << "Unknown message type from init: "
                           << subcontext_command.command_case();
        }

        if (auto result = SendMessage(init_fd_, reply); !result) {
            LOG(FATAL) << "Failed to send message to init: " << result.error();
        }
    }
}

}  // namespace

int SubcontextMain(int argc, char** argv, const BuiltinFunctionMap* function_map) {
    if (argc < 4) LOG(FATAL) << "Fewer than 4 args specified to subcontext (" << argc << ")";

    auto context = std::string(argv[2]);
    auto init_fd = std::atoi(argv[3]);

    SelabelInitialize();

    property_set = [](const std::string& key, const std::string& value) -> uint32_t {
        android::base::SetProperty(key, value);
        return 0;
    };

    auto subcontext_process = SubcontextProcess(function_map, context, init_fd);
    subcontext_process.MainLoop();
    return 0;
}

void Subcontext::Fork() {
    unique_fd subcontext_socket;
    if (!Socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, &socket_, &subcontext_socket)) {
        LOG(FATAL) << "Could not create socket pair to communicate to subcontext";
        return;
    }

    auto result = fork();

    if (result == -1) {
        LOG(FATAL) << "Could not fork subcontext";
    } else if (result == 0) {
        socket_.reset();

        // We explicitly do not use O_CLOEXEC here, such that we can reference this FD by number
        // in the subcontext process after we exec.
        int child_fd = dup(subcontext_socket);  // NOLINT(android-cloexec-dup)
        if (child_fd < 0) {
            PLOG(FATAL) << "Could not dup child_fd";
        }

        if (setexeccon(context_.c_str()) < 0) {
            PLOG(FATAL) << "Could not set execcon for '" << context_ << "'";
        }

        auto init_path = GetExecutablePath();
        auto child_fd_string = std::to_string(child_fd);
        const char* args[] = {init_path.c_str(), "subcontext", context_.c_str(),
                              child_fd_string.c_str(), nullptr};
        execv(init_path.data(), const_cast<char**>(args));

        PLOG(FATAL) << "Could not execv subcontext init";
    } else {
        subcontext_socket.reset();
        pid_ = result;
        LOG(INFO) << "Forked subcontext for '" << context_ << "' with pid " << pid_;
    }
}

void Subcontext::Restart() {
    LOG(ERROR) << "Restarting subcontext '" << context_ << "'";
    if (pid_) {
        kill(pid_, SIGKILL);
    }
    pid_ = 0;
    socket_.reset();
    Fork();
}

bool Subcontext::PathMatchesSubcontext(const std::string& path) {
    for (const auto& prefix : path_prefixes_) {
        if (StartsWith(path, prefix)) {
            return true;
        }
    }
    return false;
}

Result<SubcontextReply> Subcontext::TransmitMessage(const SubcontextCommand& subcontext_command) {
    if (auto result = SendMessage(socket_, subcontext_command); !result) {
        Restart();
        return ErrnoError() << "Failed to send message to subcontext";
    }

    auto subcontext_message = ReadMessage(socket_);
    if (!subcontext_message) {
        Restart();
        return Error() << "Failed to receive result from subcontext: " << subcontext_message.error();
    }

    auto subcontext_reply = SubcontextReply{};
    if (!subcontext_reply.ParseFromString(*subcontext_message)) {
        Restart();
        return Error() << "Unable to parse message from subcontext";
    }
    return subcontext_reply;
}

Result<void> Subcontext::Execute(const std::vector<std::string>& args) {
    auto subcontext_command = SubcontextCommand();
    std::copy(
        args.begin(), args.end(),
        RepeatedPtrFieldBackInserter(subcontext_command.mutable_execute_command()->mutable_args()));

    auto subcontext_reply = TransmitMessage(subcontext_command);
    if (!subcontext_reply) {
        return subcontext_reply.error();
    }

    if (subcontext_reply->reply_case() == SubcontextReply::kFailure) {
        auto& failure = subcontext_reply->failure();
        return ResultError(failure.error_string(), failure.error_errno());
    }

    if (subcontext_reply->reply_case() != SubcontextReply::kSuccess) {
        return Error() << "Unexpected message type from subcontext: "
                       << subcontext_reply->reply_case();
    }

    return {};
}

Result<std::vector<std::string>> Subcontext::ExpandArgs(const std::vector<std::string>& args) {
    auto subcontext_command = SubcontextCommand{};
    std::copy(args.begin(), args.end(),
              RepeatedPtrFieldBackInserter(
                  subcontext_command.mutable_expand_args_command()->mutable_args()));

    auto subcontext_reply = TransmitMessage(subcontext_command);
    if (!subcontext_reply) {
        return subcontext_reply.error();
    }

    if (subcontext_reply->reply_case() == SubcontextReply::kFailure) {
        auto& failure = subcontext_reply->failure();
        return ResultError(failure.error_string(), failure.error_errno());
    }

    if (subcontext_reply->reply_case() != SubcontextReply::kExpandArgsReply) {
        return Error() << "Unexpected message type from subcontext: "
                       << subcontext_reply->reply_case();
    }

    auto& reply = subcontext_reply->expand_args_reply();
    auto expanded_args = std::vector<std::string>{};
    for (const auto& string : reply.expanded_args()) {
        expanded_args.emplace_back(string);
    }
    return expanded_args;
}

static std::vector<Subcontext> subcontexts;
static bool shutting_down;

std::unique_ptr<Subcontext> InitializeSubcontext() {
    if (SelinuxGetVendorAndroidVersion() >= __ANDROID_API_P__) {
        return std::make_unique<Subcontext>(std::vector<std::string>{"/vendor", "/odm"},
                                            kVendorContext);
    }
    return nullptr;
}

bool SubcontextChildReap(pid_t pid) {
    for (auto& subcontext : subcontexts) {
        if (subcontext.pid() == pid) {
            if (!shutting_down) {
                subcontext.Restart();
            }
            return true;
        }
    }
    return false;
}

void SubcontextTerminate() {
    shutting_down = true;
    for (auto& subcontext : subcontexts) {
        kill(subcontext.pid(), SIGTERM);
    }
}

}  // namespace init
}  // namespace android
