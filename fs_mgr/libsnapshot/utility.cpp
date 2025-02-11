// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "utility.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::Partition;

namespace android {
namespace snapshot {

void AutoDevice::Release() {
    name_.clear();
}

AutoDeviceList::~AutoDeviceList() {
    // Destroy devices in the reverse order because newer devices may have dependencies
    // on older devices.
    for (auto it = devices_.rbegin(); it != devices_.rend(); ++it) {
        it->reset();
    }
}

void AutoDeviceList::Release() {
    for (auto&& p : devices_) {
        p->Release();
    }
}

AutoUnmapDevice::~AutoUnmapDevice() {
    if (name_.empty()) return;
    if (!dm_->DeleteDeviceIfExists(name_)) {
        LOG(ERROR) << "Failed to auto unmap device " << name_;
    }
}

AutoUnmapImage::~AutoUnmapImage() {
    if (name_.empty()) return;
    if (!images_->UnmapImageIfExists(name_)) {
        LOG(ERROR) << "Failed to auto unmap cow image " << name_;
    }
}

std::vector<Partition*> ListPartitionsWithSuffix(MetadataBuilder* builder,
                                                 const std::string& suffix) {
    std::vector<Partition*> ret;
    for (const auto& group : builder->ListGroups()) {
        for (auto* partition : builder->ListPartitionsInGroup(group)) {
            if (!base::EndsWith(partition->name(), suffix)) {
                continue;
            }
            ret.push_back(partition);
        }
    }
    return ret;
}

AutoDeleteSnapshot::~AutoDeleteSnapshot() {
    if (!name_.empty() && !manager_->DeleteSnapshot(lock_, name_)) {
        LOG(ERROR) << "Failed to auto delete snapshot " << name_;
    }
}

bool InitializeCow(const std::string& device) {
    // When the kernel creates a persistent dm-snapshot, it requires a CoW file
    // to store the modifications. The kernel interface does not specify how
    // the CoW is used, and there is no standard associated.
    // By looking at the current implementation, the CoW file is treated as:
    // - a _NEW_ snapshot if its first 32 bits are zero, so the newly created
    // dm-snapshot device will look like a perfect copy of the origin device;
    // - an _EXISTING_ snapshot if the first 32 bits are equal to a
    // kernel-specified magic number and the CoW file metadata is set as valid,
    // so it can be used to resume the last state of a snapshot device;
    // - an _INVALID_ snapshot otherwise.
    // To avoid zero-filling the whole CoW file when a new dm-snapshot is
    // created, here we zero-fill only the first 32 bits. This is a temporary
    // workaround that will be discussed again when the kernel API gets
    // consolidated.
    // TODO(b/139202197): Remove this hack once the kernel API is consolidated.
    constexpr ssize_t kDmSnapZeroFillSize = 4;  // 32-bit

    char zeros[kDmSnapZeroFillSize] = {0};
    android::base::unique_fd fd(open(device.c_str(), O_WRONLY | O_BINARY));
    if (fd < 0) {
        PLOG(ERROR) << "Can't open COW device: " << device;
        return false;
    }

    LOG(INFO) << "Zero-filling COW device: " << device;
    if (!android::base::WriteFully(fd, zeros, kDmSnapZeroFillSize)) {
        PLOG(ERROR) << "Can't zero-fill COW device for " << device;
        return false;
    }
    return true;
}

}  // namespace snapshot
}  // namespace android
