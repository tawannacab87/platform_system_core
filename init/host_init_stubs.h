/*
 * Copyright (C) 2018 The Android Open Source Project
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

#pragma once

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <string>

#include <android-base/properties.h>

// android/api-level.h
#define __ANDROID_API_P__ 28
#define __ANDROID_API_R__ 30

// sys/system_properties.h
#define PROP_VALUE_MAX 92

namespace android {
namespace init {

// init.h
inline void EnterShutdown(const std::string&) {
    abort();
}

// property_service.h
inline bool CanReadProperty(const std::string&, const std::string&) {
    return true;
}
inline uint32_t SetProperty(const std::string& key, const std::string& value) {
    android::base::SetProperty(key, value);
    return 0;
}
inline uint32_t (*property_set)(const std::string& name, const std::string& value) = SetProperty;
inline uint32_t HandlePropertySet(const std::string&, const std::string&, const std::string&,
                                  const ucred&, std::string*) {
    return 0;
}

// reboot_utils.h
inline void SetFatalRebootTarget() {}
inline void __attribute__((noreturn)) InitFatalReboot() {
    abort();
}

// selabel.h
inline void SelabelInitialize() {}
inline bool SelabelLookupFileContext(const std::string&, int, std::string*) {
    return false;
}

// selinux.h
inline int SelinuxGetVendorAndroidVersion() {
    return 10000;
}

}  // namespace init
}  // namespace android
