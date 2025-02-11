/*
 * Copyright 2016, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <debuggerd/client.h>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <iomanip>

#include <android-base/cmsg.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>
#include <procinfo/process.h>

#include "debuggerd/handler.h"
#include "protocol.h"
#include "util.h"

using namespace std::chrono_literals;

using android::base::ReadFileToString;
using android::base::SendFileDescriptors;
using android::base::unique_fd;
using android::base::WriteStringToFd;

static bool send_signal(pid_t pid, const DebuggerdDumpType dump_type) {
  const int signal = (dump_type == kDebuggerdJavaBacktrace) ? SIGQUIT : DEBUGGER_SIGNAL;
  sigval val;
  val.sival_int = (dump_type == kDebuggerdNativeBacktrace) ? 1 : 0;

  if (sigqueue(pid, signal, val) != 0) {
    PLOG(ERROR) << "libdebuggerd_client: failed to send signal to pid " << pid;
    return false;
  }
  return true;
}

template <typename Duration>
static void populate_timeval(struct timeval* tv, const Duration& duration) {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
  auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
  tv->tv_sec = static_cast<long>(seconds.count());
  tv->tv_usec = static_cast<long>(microseconds.count());
}

static void get_wchan_header(pid_t pid, std::stringstream& buffer) {
  struct tm now;
  time_t t = time(nullptr);
  localtime_r(&t, &now);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &now);
  std::string time_now(timestamp);

  std::string path = "/proc/" + std::to_string(pid) + "/cmdline";

  char proc_name_buf[1024];
  const char* proc_name = nullptr;
  std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(path.c_str(), "r"), &fclose);

  if (fp) {
    proc_name = fgets(proc_name_buf, sizeof(proc_name_buf), fp.get());
  }

  if (!proc_name) {
    proc_name = "<unknown>";
  }

  buffer << "\n----- Waiting Channels: pid " << pid << " at " << time_now << " -----\n"
         << "Cmd line: " << proc_name << "\n";
}

static void get_wchan_footer(pid_t pid, std::stringstream& buffer) {
  buffer << "----- end " << std::to_string(pid) << " -----\n";
}

/**
 * Returns the wchan data for each thread in the process,
 * or empty string if unable to obtain any data.
 */
static std::string get_wchan_data(pid_t pid) {
  std::stringstream buffer;
  std::vector<pid_t> tids;

  if (!android::procinfo::GetProcessTids(pid, &tids)) {
    LOG(WARNING) << "libdebuggerd_client: Failed to get process tids";
    return buffer.str();
  }

  std::stringstream data;
  for (int tid : tids) {
    std::string path = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/wchan";
    std::string wchan_str;
    if (!ReadFileToString(path, &wchan_str, true)) {
      PLOG(WARNING) << "libdebuggerd_client: Failed to read \"" << path << "\"";
      continue;
    }
    data << "sysTid=" << std::left << std::setw(10) << tid << wchan_str << "\n";
  }

  if (std::string str = data.str(); !str.empty()) {
    get_wchan_header(pid, buffer);
    buffer << "\n" << str << "\n";
    get_wchan_footer(pid, buffer);
    buffer << "\n";
  }

  return buffer.str();
}

static void dump_wchan_data(const std::string& data, int fd, pid_t pid) {
  if (!WriteStringToFd(data, fd)) {
    LOG(WARNING) << "libdebuggerd_client: Failed to dump wchan data for pid: " << pid;
  }
}

bool debuggerd_trigger_dump(pid_t tid, DebuggerdDumpType dump_type, unsigned int timeout_ms,
                            unique_fd output_fd) {
  pid_t pid = tid;
  if (dump_type == kDebuggerdJavaBacktrace) {
    // Java dumps always get sent to the tgid, so we need to resolve our tid to a tgid.
    android::procinfo::ProcessInfo procinfo;
    std::string error;
    if (!android::procinfo::GetProcessInfo(tid, &procinfo, &error)) {
      LOG(ERROR) << "libdebugged_client: failed to get process info: " << error;
      return false;
    }
    pid = procinfo.pid;
  }

  LOG(INFO) << "libdebuggerd_client: started dumping process " << pid;
  unique_fd sockfd;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  auto time_left = [&end]() { return end - std::chrono::steady_clock::now(); };
  auto set_timeout = [timeout_ms, &time_left](int sockfd) {
    if (timeout_ms <= 0) {
      return sockfd;
    }

    auto remaining = time_left();
    if (remaining < decltype(remaining)::zero()) {
      LOG(ERROR) << "libdebuggerd_client: timeout expired";
      return -1;
    }

    struct timeval timeout;
    populate_timeval(&timeout, remaining);

    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
      PLOG(ERROR) << "libdebuggerd_client: failed to set receive timeout";
      return -1;
    }
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
      PLOG(ERROR) << "libdebuggerd_client: failed to set send timeout";
      return -1;
    }

    return sockfd;
  };

  sockfd.reset(socket(AF_LOCAL, SOCK_SEQPACKET, 0));
  if (sockfd == -1) {
    PLOG(ERROR) << "libdebugger_client: failed to create socket";
    return false;
  }

  if (socket_local_client_connect(set_timeout(sockfd.get()), kTombstonedInterceptSocketName,
                                  ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_SEQPACKET) == -1) {
    PLOG(ERROR) << "libdebuggerd_client: failed to connect to tombstoned";
    return false;
  }

  InterceptRequest req = {.pid = pid, .dump_type = dump_type};
  if (!set_timeout(sockfd)) {
    PLOG(ERROR) << "libdebugger_client: failed to set timeout";
    return false;
  }

  // Create an intermediate pipe to pass to the other end.
  unique_fd pipe_read, pipe_write;
  if (!Pipe(&pipe_read, &pipe_write)) {
    PLOG(ERROR) << "libdebuggerd_client: failed to create pipe";
    return false;
  }

  std::string pipe_size_str;
  int pipe_buffer_size = 1024 * 1024;
  if (android::base::ReadFileToString("/proc/sys/fs/pipe-max-size", &pipe_size_str)) {
    pipe_size_str = android::base::Trim(pipe_size_str);

    if (!android::base::ParseInt(pipe_size_str.c_str(), &pipe_buffer_size, 0)) {
      LOG(FATAL) << "failed to parse pipe max size '" << pipe_size_str << "'";
    }
  }

  if (fcntl(pipe_read.get(), F_SETPIPE_SZ, pipe_buffer_size) != pipe_buffer_size) {
    PLOG(ERROR) << "failed to set pipe buffer size";
  }

  ssize_t rc = SendFileDescriptors(set_timeout(sockfd), &req, sizeof(req), pipe_write.get());
  pipe_write.reset();
  if (rc != sizeof(req)) {
    PLOG(ERROR) << "libdebuggerd_client: failed to send output fd to tombstoned";
    return false;
  }

  // Check to make sure we've successfully registered.
  InterceptResponse response;
  rc = TEMP_FAILURE_RETRY(recv(set_timeout(sockfd.get()), &response, sizeof(response), MSG_TRUNC));
  if (rc == 0) {
    LOG(ERROR) << "libdebuggerd_client: failed to read initial response from tombstoned: "
               << "timeout reached?";
    return false;
  } else if (rc == -1) {
    PLOG(ERROR) << "libdebuggerd_client: failed to read initial response from tombstoned";
    return false;
  } else if (rc != sizeof(response)) {
    LOG(ERROR) << "libdebuggerd_client: received packet of unexpected length from tombstoned while "
                  "reading initial response: expected "
               << sizeof(response) << ", received " << rc;
    return false;
  }

  if (response.status != InterceptStatus::kRegistered) {
    LOG(ERROR) << "libdebuggerd_client: unexpected registration response: "
               << static_cast<int>(response.status);
    return false;
  }

  if (!send_signal(tid, dump_type)) {
    return false;
  }

  rc = TEMP_FAILURE_RETRY(recv(set_timeout(sockfd.get()), &response, sizeof(response), MSG_TRUNC));
  if (rc == 0) {
    LOG(ERROR) << "libdebuggerd_client: failed to read status response from tombstoned: "
                  "timeout reached?";
    return false;
  } else if (rc == -1) {
    PLOG(ERROR) << "libdebuggerd_client: failed to read status response from tombstoned";
    return false;
  } else if (rc != sizeof(response)) {
    LOG(ERROR) << "libdebuggerd_client: received packet of unexpected length from tombstoned while "
                  "reading confirmation response: expected "
               << sizeof(response) << ", received " << rc;
    return false;
  }

  if (response.status != InterceptStatus::kStarted) {
    response.error_message[sizeof(response.error_message) - 1] = '\0';
    LOG(ERROR) << "libdebuggerd_client: tombstoned reported failure: " << response.error_message;
    return false;
  }

  // Forward output from the pipe to the output fd.
  while (true) {
    auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_left()).count();
    if (timeout_ms <= 0) {
      remaining_ms = -1;
    } else if (remaining_ms < 0) {
      LOG(ERROR) << "libdebuggerd_client: timeout expired";
      return false;
    }

    struct pollfd pfd = {
        .fd = pipe_read.get(), .events = POLLIN, .revents = 0,
    };

    rc = poll(&pfd, 1, remaining_ms);
    if (rc == -1) {
      if (errno == EINTR) {
        continue;
      } else {
        PLOG(ERROR) << "libdebuggerd_client: error while polling";
        return false;
      }
    } else if (rc == 0) {
      LOG(ERROR) << "libdebuggerd_client: timeout expired";
      return false;
    }

    char buf[1024];
    rc = TEMP_FAILURE_RETRY(read(pipe_read.get(), buf, sizeof(buf)));
    if (rc == 0) {
      // Done.
      break;
    } else if (rc == -1) {
      PLOG(ERROR) << "libdebuggerd_client: error while reading";
      return false;
    }

    if (!android::base::WriteFully(output_fd.get(), buf, rc)) {
      PLOG(ERROR) << "libdebuggerd_client: error while writing";
      return false;
    }
  }

  LOG(INFO) << "libdebuggerd_client: done dumping process " << pid;

  return true;
}

int dump_backtrace_to_file(pid_t tid, DebuggerdDumpType dump_type, int fd) {
  return dump_backtrace_to_file_timeout(tid, dump_type, 0, fd);
}

int dump_backtrace_to_file_timeout(pid_t tid, DebuggerdDumpType dump_type, int timeout_secs,
                                   int fd) {
  android::base::unique_fd copy(dup(fd));
  if (copy == -1) {
    return -1;
  }

  // debuggerd_trigger_dump results in every thread in the process being interrupted
  // by a signal, so we need to fetch the wchan data before calling that.
  std::string wchan_data = get_wchan_data(tid);

  int timeout_ms = timeout_secs > 0 ? timeout_secs * 1000 : 0;
  int ret = debuggerd_trigger_dump(tid, dump_type, timeout_ms, std::move(copy)) ? 0 : -1;

  // Dump wchan data, since only privileged processes (CAP_SYS_ADMIN) can read
  // kernel stack traces (/proc/*/stack).
  dump_wchan_data(wchan_data, fd, tid);

  return ret;
}
