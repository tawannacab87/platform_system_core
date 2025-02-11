/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <functional>

#include <android-base/file.h>
#include <android-base/macros.h>
#include <android-base/unique_fd.h>
#include <gtest/gtest.h>

#include <modprobe/modprobe.h>

#include "libmodprobe_test.h"

// Used by libmodprobe_ext_test to check if requested modules are present.
std::vector<std::string> test_modules;

// Used by libmodprobe_ext_test to report which modules would have been loaded.
std::vector<std::string> modules_loaded;

TEST(libmodprobe, Test) {
    test_modules = {
            "/test1.ko",  "/test2.ko",  "/test3.ko",  "/test4.ko",  "/test5.ko",
            "/test6.ko",  "/test7.ko",  "/test8.ko",  "/test9.ko",  "/test10.ko",
            "/test11.ko", "/test12.ko", "/test13.ko", "/test14.ko", "/test15.ko",
    };

    std::vector<std::string> expected_modules_loaded = {
            "/test14.ko",
            "/test15.ko",
            "/test3.ko",
            "/test4.ko",
            "/test1.ko",
            "/test6.ko",
            "/test2.ko",
            "/test5.ko",
            "/test8.ko",
            "/test7.ko param1=4",
            "/test9.ko param_x=1 param_y=2 param_z=3",
            "/test10.ko",
            "/test12.ko",
            "/test11.ko",
            "/test13.ko",
    };

    std::vector<std::string> expected_after_remove = {
            "/test14.ko", "/test15.ko",         "/test1.ko",
            "/test6.ko",  "/test2.ko",          "/test5.ko",
            "/test8.ko",  "/test7.ko param1=4", "/test9.ko param_x=1 param_y=2 param_z=3",
            "/test10.ko", "/test12.ko",         "/test11.ko",
            "/test13.ko",
    };

    const std::string modules_dep =
            "test1.ko:\n"
            "test2.ko:\n"
            "test3.ko:\n"
            "test4.ko: test3.ko\n"
            "test5.ko: test2.ko test6.ko\n"
            "test6.ko:\n"
            "test7.ko:\n"
            "test8.ko:\n"
            "test9.ko:\n"
            "test10.ko:\n"
            "test11.ko:\n"
            "test12.ko:\n"
            "test13.ko:\n"
            "test14.ko:\n"
            "test15.ko:\n";

    const std::string modules_softdep =
            "softdep test7 pre: test8\n"
            "softdep test9 post: test10\n"
            "softdep test11 pre: test12 post: test13\n"
            "softdep test3 pre: test141516\n";

    const std::string modules_alias =
            "# Aliases extracted from modules themselves.\n"
            "\n"
            "alias test141516 test14\n"
            "alias test141516 test15\n"
            "alias test141516 test16\n";

    const std::string modules_options =
            "options test7.ko param1=4\n"
            "options test9.ko param_x=1 param_y=2 param_z=3\n"
            "options test100.ko param_1=1\n";

    const std::string modules_blacklist =
            "blacklist test9.ko\n"
            "blacklist test3.ko\n";

    const std::string modules_load =
            "test4.ko\n"
            "test1.ko\n"
            "test3.ko\n"
            "test5.ko\n"
            "test7.ko\n"
            "test9.ko\n"
            "test11.ko\n";

    TemporaryDir dir;
    auto dir_path = std::string(dir.path);
    ASSERT_TRUE(android::base::WriteStringToFile(modules_alias, dir_path + "/modules.alias", 0600,
                                                 getuid(), getgid()));

    ASSERT_TRUE(android::base::WriteStringToFile(modules_dep, dir_path + "/modules.dep", 0600,
                                                 getuid(), getgid()));
    ASSERT_TRUE(android::base::WriteStringToFile(modules_softdep, dir_path + "/modules.softdep",
                                                 0600, getuid(), getgid()));
    ASSERT_TRUE(android::base::WriteStringToFile(modules_options, dir_path + "/modules.options",
                                                 0600, getuid(), getgid()));
    ASSERT_TRUE(android::base::WriteStringToFile(modules_load, dir_path + "/modules.load", 0600,
                                                 getuid(), getgid()));
    ASSERT_TRUE(android::base::WriteStringToFile(modules_blacklist, dir_path + "/modules.blacklist",
                                                 0600, getuid(), getgid()));

    for (auto i = test_modules.begin(); i != test_modules.end(); ++i) {
        *i = dir.path + *i;
    }

    Modprobe m({dir.path});
    EXPECT_TRUE(m.LoadListedModules());

    GTEST_LOG_(INFO) << "Expected modules loaded (in order):";
    for (auto i = expected_modules_loaded.begin(); i != expected_modules_loaded.end(); ++i) {
        *i = dir.path + *i;
        GTEST_LOG_(INFO) << "\"" << *i << "\"";
    }
    GTEST_LOG_(INFO) << "Actual modules loaded (in order):";
    for (auto i = modules_loaded.begin(); i != modules_loaded.end(); ++i) {
        GTEST_LOG_(INFO) << "\"" << *i << "\"";
    }

    EXPECT_TRUE(modules_loaded == expected_modules_loaded);

    EXPECT_TRUE(m.Remove("test4"));

    GTEST_LOG_(INFO) << "Expected modules loaded after removing test4 (in order):";
    for (auto i = expected_after_remove.begin(); i != expected_after_remove.end(); ++i) {
        *i = dir.path + *i;
        GTEST_LOG_(INFO) << "\"" << *i << "\"";
    }
    GTEST_LOG_(INFO) << "Actual modules loaded after removing test4 (in order):";
    for (auto i = modules_loaded.begin(); i != modules_loaded.end(); ++i) {
        GTEST_LOG_(INFO) << "\"" << *i << "\"";
    }

    EXPECT_TRUE(modules_loaded == expected_after_remove);

    m.EnableBlacklist(true);
    EXPECT_FALSE(m.LoadWithAliases("test4", true));
}
