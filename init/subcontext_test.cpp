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

#include <unistd.h>

#include <chrono>

#include <android-base/properties.h>
#include <android-base/strings.h>
#include <gtest/gtest.h>
#include <selinux/selinux.h>

#include "builtin_arguments.h"

using namespace std::literals;

using android::base::GetProperty;
using android::base::Join;
using android::base::SetProperty;
using android::base::Split;
using android::base::WaitForProperty;

namespace android {
namespace init {

// I would use test fixtures, but I cannot skip the test if not root with them, so instead we have
// this test runner.
template <typename F>
void RunTest(F&& test_function) {
    if (getuid() != 0) {
        GTEST_SKIP() << "Skipping test, must be run as root.";
        return;
    }

    char* context;
    ASSERT_EQ(0, getcon(&context));
    auto context_string = std::string(context);
    free(context);

    auto subcontext = Subcontext({"dummy_path"}, context_string);
    ASSERT_NE(0, subcontext.pid());

    test_function(subcontext, context_string);

    if (subcontext.pid() > 0) {
        kill(subcontext.pid(), SIGTERM);
        kill(subcontext.pid(), SIGKILL);
    }
}

TEST(subcontext, CheckDifferentPid) {
    RunTest([](auto& subcontext, auto& context_string) {
        auto result = subcontext.Execute(std::vector<std::string>{"return_pids_as_error"});
        ASSERT_FALSE(result);

        auto pids = Split(result.error().message(), " ");
        ASSERT_EQ(2U, pids.size());
        auto our_pid = std::to_string(getpid());
        EXPECT_NE(our_pid, pids[0]);
        EXPECT_EQ(our_pid, pids[1]);
    });
}

TEST(subcontext, SetProp) {
    RunTest([](auto& subcontext, auto& context_string) {
        SetProperty("init.test.subcontext", "fail");
        WaitForProperty("init.test.subcontext", "fail");

        auto args = std::vector<std::string>{
            "setprop",
            "init.test.subcontext",
            "success",
        };
        auto result = subcontext.Execute(args);
        ASSERT_TRUE(result) << result.error();

        EXPECT_TRUE(WaitForProperty("init.test.subcontext", "success", 10s));
    });
}

TEST(subcontext, MultipleCommands) {
    RunTest([](auto& subcontext, auto& context_string) {
        auto first_pid = subcontext.pid();

        auto expected_words = std::vector<std::string>{
            "this",
            "is",
            "a",
            "test",
        };

        for (const auto& word : expected_words) {
            auto args = std::vector<std::string>{
                "add_word",
                word,
            };
            auto result = subcontext.Execute(args);
            ASSERT_TRUE(result) << result.error();
        }

        auto result = subcontext.Execute(std::vector<std::string>{"return_words_as_error"});
        ASSERT_FALSE(result);
        EXPECT_EQ(Join(expected_words, " "), result.error().message());
        EXPECT_EQ(first_pid, subcontext.pid());
    });
}

TEST(subcontext, RecoverAfterAbort) {
    RunTest([](auto& subcontext, auto& context_string) {
        auto first_pid = subcontext.pid();

        auto result = subcontext.Execute(std::vector<std::string>{"cause_log_fatal"});
        ASSERT_FALSE(result);

        auto result2 = subcontext.Execute(std::vector<std::string>{"generate_sane_error"});
        ASSERT_FALSE(result2);
        EXPECT_EQ("Sane error!", result2.error().message());
        EXPECT_NE(subcontext.pid(), first_pid);
    });
}

TEST(subcontext, ContextString) {
    RunTest([](auto& subcontext, auto& context_string) {
        auto result = subcontext.Execute(std::vector<std::string>{"return_context_as_error"});
        ASSERT_FALSE(result);
        ASSERT_EQ(context_string, result.error().message());
    });
}

TEST(subcontext, ExpandArgs) {
    RunTest([](auto& subcontext, auto& context_string) {
        auto args = std::vector<std::string>{
            "first",
            "${ro.hardware}",
            "$$third",
        };
        auto result = subcontext.ExpandArgs(args);
        ASSERT_TRUE(result) << result.error();
        ASSERT_EQ(3U, result->size());
        EXPECT_EQ(args[0], result->at(0));
        EXPECT_EQ(GetProperty("ro.hardware", ""), result->at(1));
        EXPECT_EQ("$third", result->at(2));
    });
}

TEST(subcontext, ExpandArgsFailure) {
    RunTest([](auto& subcontext, auto& context_string) {
        auto args = std::vector<std::string>{
            "first",
            "${",
        };
        auto result = subcontext.ExpandArgs(args);
        ASSERT_FALSE(result);
        EXPECT_EQ("unexpected end of string in '" + args[1] + "', looking for }",
                  result.error().message());
    });
}

BuiltinFunctionMap BuildTestFunctionMap() {
    // For CheckDifferentPid
    auto do_return_pids_as_error = [](const BuiltinArguments& args) -> Result<void> {
        return Error() << getpid() << " " << getppid();
    };

    // For SetProp
    auto do_setprop = [](const BuiltinArguments& args) {
        android::base::SetProperty(args[1], args[2]);
        return Result<void>{};
    };

    // For MultipleCommands
    // Using a shared_ptr to extend lifetime of words to both lambdas
    auto words = std::make_shared<std::vector<std::string>>();
    auto do_add_word = [words](const BuiltinArguments& args) {
        words->emplace_back(args[1]);
        return Result<void>{};
    };
    auto do_return_words_as_error = [words](const BuiltinArguments& args) -> Result<void> {
        return Error() << Join(*words, " ");
    };

    // For RecoverAfterAbort
    auto do_cause_log_fatal = [](const BuiltinArguments& args) -> Result<void> {
        return Error() << std::string(4097, 'f');
    };
    auto do_generate_sane_error = [](const BuiltinArguments& args) -> Result<void> {
        return Error() << "Sane error!";
    };

    // For ContextString
    auto do_return_context_as_error = [](const BuiltinArguments& args) -> Result<void> {
        return Error() << args.context;
    };

    // clang-format off
    BuiltinFunctionMap test_function_map = {
        {"return_pids_as_error",        {0,     0,      {true,  do_return_pids_as_error}}},
        {"setprop",                     {2,     2,      {true,  do_setprop}}},
        {"add_word",                    {1,     1,      {true,  do_add_word}}},
        {"return_words_as_error",       {0,     0,      {true,  do_return_words_as_error}}},
        {"cause_log_fatal",             {0,     0,      {true,  do_cause_log_fatal}}},
        {"generate_sane_error",         {0,     0,      {true,  do_generate_sane_error}}},
        {"return_context_as_error",     {0,     0,      {true,  do_return_context_as_error}}},
    };
    // clang-format on
    return test_function_map;
}

}  // namespace init
}  // namespace android

// init_test.cpp contains the main entry point for all init tests.
int SubcontextTestChildMain(int argc, char** argv) {
    auto test_function_map = android::init::BuildTestFunctionMap();
    return android::init::SubcontextMain(argc, argv, &test_function_map);
}
