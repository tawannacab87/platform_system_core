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

#include <libsnapshot/snapshot.h>

#include <dirent.h>
#include <math.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/unistd.h>

#include <optional>
#include <thread>
#include <unordered_set>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr.h>
#include <fs_mgr_dm_linear.h>
#include <fs_mgr_overlayfs.h>
#include <fstab/fstab.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/liblp.h>

#include "partition_cow_creator.h"
#include "snapshot_metadata_updater.h"
#include "utility.h"

namespace android {
namespace snapshot {

using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::dm::DmTargetSnapshot;
using android::dm::kSectorSize;
using android::dm::SnapshotStorageMode;
using android::fiemap::IImageManager;
using android::fs_mgr::CreateDmTable;
using android::fs_mgr::CreateLogicalPartition;
using android::fs_mgr::CreateLogicalPartitionParams;
using android::fs_mgr::GetPartitionGroupName;
using android::fs_mgr::GetPartitionName;
using android::fs_mgr::LpMetadata;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::SlotNumberForSlotSuffix;
using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::InstallOperation;
template <typename T>
using RepeatedPtrField = google::protobuf::RepeatedPtrField<T>;
using std::chrono::duration_cast;
using namespace std::chrono_literals;
using namespace std::string_literals;

// Unit is sectors, this is a 4K chunk.
static constexpr uint32_t kSnapshotChunkSize = 8;
static constexpr char kBootIndicatorPath[] = "/metadata/ota/snapshot-boot";

class DeviceInfo final : public SnapshotManager::IDeviceInfo {
  public:
    std::string GetGsidDir() const override { return "ota"s; }
    std::string GetMetadataDir() const override { return "/metadata/ota"s; }
    std::string GetSlotSuffix() const override { return fs_mgr_get_slot_suffix(); }
    std::string GetOtherSlotSuffix() const override { return fs_mgr_get_other_slot_suffix(); }
    const android::fs_mgr::IPartitionOpener& GetPartitionOpener() const { return opener_; }
    std::string GetSuperDevice(uint32_t slot) const override {
        return fs_mgr_get_super_partition_name(slot);
    }
    bool IsOverlayfsSetup() const override { return fs_mgr_overlayfs_is_setup(); }

  private:
    android::fs_mgr::PartitionOpener opener_;
};

// Note: IImageManager is an incomplete type in the header, so the default
// destructor doesn't work.
SnapshotManager::~SnapshotManager() {}

std::unique_ptr<SnapshotManager> SnapshotManager::New(IDeviceInfo* info) {
    if (!info) {
        info = new DeviceInfo();
    }
    return std::unique_ptr<SnapshotManager>(new SnapshotManager(info));
}

std::unique_ptr<SnapshotManager> SnapshotManager::NewForFirstStageMount(IDeviceInfo* info) {
    auto sm = New(info);
    if (!sm || !sm->ForceLocalImageManager()) {
        return nullptr;
    }
    return sm;
}

SnapshotManager::SnapshotManager(IDeviceInfo* device) : device_(device) {
    gsid_dir_ = device_->GetGsidDir();
    metadata_dir_ = device_->GetMetadataDir();
}

static std::string GetCowName(const std::string& snapshot_name) {
    return snapshot_name + "-cow";
}

static std::string GetCowImageDeviceName(const std::string& snapshot_name) {
    return snapshot_name + "-cow-img";
}

static std::string GetBaseDeviceName(const std::string& partition_name) {
    return partition_name + "-base";
}

static std::string GetSnapshotExtraDeviceName(const std::string& snapshot_name) {
    return snapshot_name + "-inner";
}

bool SnapshotManager::BeginUpdate() {
    bool needs_merge = false;
    if (!TryCancelUpdate(&needs_merge)) {
        return false;
    }
    if (needs_merge) {
        LOG(INFO) << "Wait for merge (if any) before beginning a new update.";
        auto state = ProcessUpdateState();
        LOG(INFO) << "Merged with state = " << state;
    }

    auto file = LockExclusive();
    if (!file) return false;

    auto state = ReadUpdateState(file.get());
    if (state != UpdateState::None) {
        LOG(ERROR) << "An update is already in progress, cannot begin a new update";
        return false;
    }
    return WriteUpdateState(file.get(), UpdateState::Initiated);
}

bool SnapshotManager::CancelUpdate() {
    bool needs_merge = false;
    if (!TryCancelUpdate(&needs_merge)) {
        return false;
    }
    if (needs_merge) {
        LOG(ERROR) << "Cannot cancel update after it has completed or started merging";
    }
    return !needs_merge;
}

bool SnapshotManager::TryCancelUpdate(bool* needs_merge) {
    *needs_merge = false;

    auto file = LockExclusive();
    if (!file) return false;

    UpdateState state = ReadUpdateState(file.get());
    if (state == UpdateState::None) return true;

    if (state == UpdateState::Initiated) {
        LOG(INFO) << "Update has been initiated, now canceling";
        return RemoveAllUpdateState(file.get());
    }

    if (state == UpdateState::Unverified) {
        // We completed an update, but it can still be canceled if we haven't booted into it.
        auto boot_file = GetSnapshotBootIndicatorPath();
        std::string contents;
        if (!android::base::ReadFileToString(boot_file, &contents)) {
            PLOG(WARNING) << "Cannot read " << boot_file << ", proceed to canceling the update:";
            return RemoveAllUpdateState(file.get());
        }
        if (device_->GetSlotSuffix() == contents) {
            LOG(INFO) << "Canceling a previously completed update";
            return RemoveAllUpdateState(file.get());
        }
    }
    *needs_merge = true;
    return true;
}

bool SnapshotManager::RemoveAllUpdateState(LockedFile* lock) {
    if (!RemoveAllSnapshots(lock)) {
        LOG(ERROR) << "Could not remove all snapshots";
        return false;
    }

    RemoveSnapshotBootIndicator();

    // If this fails, we'll keep trying to remove the update state (as the
    // device reboots or starts a new update) until it finally succeeds.
    return WriteUpdateState(lock, UpdateState::None);
}

bool SnapshotManager::FinishedSnapshotWrites() {
    auto lock = LockExclusive();
    if (!lock) return false;

    auto update_state = ReadUpdateState(lock.get());
    if (update_state == UpdateState::Unverified) {
        LOG(INFO) << "FinishedSnapshotWrites already called before. Ignored.";
        return true;
    }

    if (update_state != UpdateState::Initiated) {
        LOG(ERROR) << "Can only transition to the Unverified state from the Initiated state.";
        return false;
    }

    // This file acts as both a quick indicator for init (it can use access(2)
    // to decide how to do first-stage mounts), and it stores the old slot, so
    // we can tell whether or not we performed a rollback.
    auto contents = device_->GetSlotSuffix();
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (!android::base::WriteStringToFile(contents, boot_file)) {
        PLOG(ERROR) << "write failed: " << boot_file;
        return false;
    }
    return WriteUpdateState(lock.get(), UpdateState::Unverified);
}

bool SnapshotManager::CreateSnapshot(LockedFile* lock, const std::string& name,
                                     SnapshotManager::SnapshotStatus status) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    // Sanity check these sizes. Like liblp, we guarantee the partition size
    // is respected, which means it has to be sector-aligned. (This guarantee
    // is useful for locating avb footers correctly). The COW file size, however,
    // can be arbitrarily larger than specified, so we can safely round it up.
    if (status.device_size % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name
                   << " device size is not a multiple of the sector size: " << status.device_size;
        return false;
    }
    if (status.snapshot_size % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name << " snapshot size is not a multiple of the sector size: "
                   << status.snapshot_size;
        return false;
    }
    if (status.cow_partition_size % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name
                   << " cow partition size is not a multiple of the sector size: "
                   << status.cow_partition_size;
        return false;
    }
    if (status.cow_file_size % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name << " cow file size is not a multiple of the sector size: "
                   << status.cow_partition_size;
        return false;
    }

    status.state = SnapshotState::Created;
    status.sectors_allocated = 0;
    status.metadata_sectors = 0;

    if (!WriteSnapshotStatus(lock, name, status)) {
        PLOG(ERROR) << "Could not write snapshot status: " << name;
        return false;
    }
    return true;
}

bool SnapshotManager::CreateCowImage(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    if (!EnsureImageManager()) return false;

    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return false;
    }

    // The COW file size should have been rounded up to the nearest sector in CreateSnapshot.
    // Sanity check this.
    if (status.cow_file_size % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name << " COW file size is not a multiple of the sector size: "
                   << status.cow_file_size;
        return false;
    }

    std::string cow_image_name = GetCowImageDeviceName(name);
    int cow_flags = IImageManager::CREATE_IMAGE_DEFAULT;
    return images_->CreateBackingImage(cow_image_name, status.cow_file_size, cow_flags);
}

bool SnapshotManager::MapSnapshot(LockedFile* lock, const std::string& name,
                                  const std::string& base_device, const std::string& cow_device,
                                  const std::chrono::milliseconds& timeout_ms,
                                  std::string* dev_path) {
    CHECK(lock);
    if (!EnsureImageManager()) return false;

    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return false;
    }
    if (status.state == SnapshotState::MergeCompleted) {
        LOG(ERROR) << "Should not create a snapshot device for " << name
                   << " after merging has completed.";
        return false;
    }

    // Validate the block device size, as well as the requested snapshot size.
    // Note that during first-stage init, we don't have the device paths.
    if (android::base::StartsWith(base_device, "/")) {
        unique_fd fd(open(base_device.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            PLOG(ERROR) << "open failed: " << base_device;
            return false;
        }
        auto dev_size = get_block_device_size(fd);
        if (!dev_size) {
            PLOG(ERROR) << "Could not determine block device size: " << base_device;
            return false;
        }
        if (status.device_size != dev_size) {
            LOG(ERROR) << "Block device size for " << base_device << " does not match"
                       << "(expected " << status.device_size << ", got " << dev_size << ")";
            return false;
        }
    }
    if (status.device_size % kSectorSize != 0) {
        LOG(ERROR) << "invalid blockdev size for " << base_device << ": " << status.device_size;
        return false;
    }
    if (status.snapshot_size % kSectorSize != 0 || status.snapshot_size > status.device_size) {
        LOG(ERROR) << "Invalid snapshot size for " << base_device << ": " << status.snapshot_size;
        return false;
    }
    uint64_t snapshot_sectors = status.snapshot_size / kSectorSize;
    uint64_t linear_sectors = (status.device_size - status.snapshot_size) / kSectorSize;



    auto& dm = DeviceMapper::Instance();

    // Note that merging is a global state. We do track whether individual devices
    // have completed merging, but the start of the merge process is considered
    // atomic.
    SnapshotStorageMode mode;
    switch (ReadUpdateState(lock)) {
        case UpdateState::MergeCompleted:
        case UpdateState::MergeNeedsReboot:
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after global merging has completed.";
            return false;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            // Note: MergeFailed indicates that a merge is in progress, but
            // is possibly stalled. We still have to honor the merge.
            mode = SnapshotStorageMode::Merge;
            break;
        default:
            mode = SnapshotStorageMode::Persistent;
            break;
    }

    // The kernel (tested on 4.19) crashes horribly if a device has both a snapshot
    // and a linear target in the same table. Instead, we stack them, and give the
    // snapshot device a different name. It is not exposed to the caller in this
    // case.
    auto snap_name = (linear_sectors > 0) ? GetSnapshotExtraDeviceName(name) : name;

    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, snapshot_sectors, base_device, cow_device, mode,
                                    kSnapshotChunkSize);
    if (!dm.CreateDevice(snap_name, table, dev_path, timeout_ms)) {
        LOG(ERROR) << "Could not create snapshot device: " << snap_name;
        return false;
    }

    if (linear_sectors) {
        std::string snap_dev;
        if (!dm.GetDeviceString(snap_name, &snap_dev)) {
            LOG(ERROR) << "Cannot determine major/minor for: " << snap_name;
            return false;
        }

        // Our stacking will looks like this:
        //     [linear, linear] ; to snapshot, and non-snapshot region of base device
        //     [snapshot-inner]
        //     [base device]   [cow]
        DmTable table;
        table.Emplace<DmTargetLinear>(0, snapshot_sectors, snap_dev, 0);
        table.Emplace<DmTargetLinear>(snapshot_sectors, linear_sectors, base_device,
                                      snapshot_sectors);
        if (!dm.CreateDevice(name, table, dev_path, timeout_ms)) {
            LOG(ERROR) << "Could not create outer snapshot device: " << name;
            dm.DeleteDevice(snap_name);
            return false;
        }
    }

    // :TODO: when merging is implemented, we need to add an argument to the
    // status indicating how much progress is left to merge. (device-mapper
    // does not retain the initial values, so we can't derive them.)
    return true;
}

bool SnapshotManager::MapCowImage(const std::string& name,
                                  const std::chrono::milliseconds& timeout_ms) {
    if (!EnsureImageManager()) return false;
    auto cow_image_name = GetCowImageDeviceName(name);

    bool ok;
    std::string cow_dev;
    if (has_local_image_manager_) {
        // If we forced a local image manager, it means we don't have binder,
        // which means first-stage init. We must use device-mapper.
        const auto& opener = device_->GetPartitionOpener();
        ok = images_->MapImageWithDeviceMapper(opener, cow_image_name, &cow_dev);
    } else {
        ok = images_->MapImageDevice(cow_image_name, timeout_ms, &cow_dev);
    }

    if (ok) {
        LOG(INFO) << "Mapped " << cow_image_name << " to " << cow_dev;
    } else {
        LOG(ERROR) << "Could not map image device: " << cow_image_name;
    }
    return ok;
}

bool SnapshotManager::UnmapSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);

    auto& dm = DeviceMapper::Instance();
    if (!dm.DeleteDeviceIfExists(name)) {
        LOG(ERROR) << "Could not delete snapshot device: " << name;
        return false;
    }

    auto snapshot_extra_device = GetSnapshotExtraDeviceName(name);
    if (!dm.DeleteDeviceIfExists(snapshot_extra_device)) {
        LOG(ERROR) << "Could not delete snapshot inner device: " << snapshot_extra_device;
        return false;
    }

    return true;
}

bool SnapshotManager::UnmapCowImage(const std::string& name) {
    if (!EnsureImageManager()) return false;
    return images_->UnmapImageIfExists(GetCowImageDeviceName(name));
}

bool SnapshotManager::DeleteSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    if (!EnsureImageManager()) return false;

    if (!UnmapCowDevices(lock, name)) {
        return false;
    }

    auto cow_image_name = GetCowImageDeviceName(name);
    if (images_->BackingImageExists(cow_image_name)) {
        if (!images_->DeleteBackingImage(cow_image_name)) {
            return false;
        }
    }

    std::string error;
    auto file_path = GetSnapshotStatusFilePath(name);
    if (!android::base::RemoveFileIfExists(file_path, &error)) {
        LOG(ERROR) << "Failed to remove status file " << file_path << ": " << error;
        return false;
    }
    return true;
}

bool SnapshotManager::InitiateMerge() {
    auto lock = LockExclusive();
    if (!lock) return false;

    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Unverified) {
        LOG(ERROR) << "Cannot begin a merge if an update has not been verified";
        return false;
    }

    std::string old_slot;
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (!android::base::ReadFileToString(boot_file, &old_slot)) {
        LOG(ERROR) << "Could not determine the previous slot; aborting merge";
        return false;
    }
    auto new_slot = device_->GetSlotSuffix();
    if (new_slot == old_slot) {
        LOG(ERROR) << "Device cannot merge while booting off old slot " << old_slot;
        return false;
    }

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return false;
    }

    auto& dm = DeviceMapper::Instance();
    for (const auto& snapshot : snapshots) {
        // The device has to be mapped, since everything should be merged at
        // the same time. This is a fairly serious error. We could forcefully
        // map everything here, but it should have been mapped during first-
        // stage init.
        if (dm.GetState(snapshot) == DmDeviceState::INVALID) {
            LOG(ERROR) << "Cannot begin merge; device " << snapshot << " is not mapped.";
            return false;
        }
    }

    // Point of no return - mark that we're starting a merge. From now on every
    // snapshot must be a merge target.
    if (!WriteUpdateState(lock.get(), UpdateState::Merging)) {
        return false;
    }

    bool rewrote_all = true;
    for (const auto& snapshot : snapshots) {
        // If this fails, we have no choice but to continue. Everything must
        // be merged. This is not an ideal state to be in, but it is safe,
        // because we the next boot will try again.
        if (!SwitchSnapshotToMerge(lock.get(), snapshot)) {
            LOG(ERROR) << "Failed to switch snapshot to a merge target: " << snapshot;
            rewrote_all = false;
        }
    }

    // If we couldn't switch everything to a merge target, pre-emptively mark
    // this merge as failed. It will get acknowledged when WaitForMerge() is
    // called.
    if (!rewrote_all) {
        WriteUpdateState(lock.get(), UpdateState::MergeFailed);
    }

    // Return true no matter what, because a merge was initiated.
    return true;
}

bool SnapshotManager::SwitchSnapshotToMerge(LockedFile* lock, const std::string& name) {
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return false;
    }
    if (status.state != SnapshotState::Created) {
        LOG(WARNING) << "Snapshot " << name << " has unexpected state: " << to_string(status.state);
    }

    // After this, we return true because we technically did switch to a merge
    // target. Everything else we do here is just informational.
    auto dm_name = GetSnapshotDeviceName(name, status);
    if (!RewriteSnapshotDeviceTable(dm_name)) {
        return false;
    }

    status.state = SnapshotState::Merging;

    DmTargetSnapshot::Status dm_status;
    if (!QuerySnapshotStatus(dm_name, nullptr, &dm_status)) {
        LOG(ERROR) << "Could not query merge status for snapshot: " << dm_name;
    }
    status.sectors_allocated = dm_status.sectors_allocated;
    status.metadata_sectors = dm_status.metadata_sectors;
    if (!WriteSnapshotStatus(lock, name, status)) {
        LOG(ERROR) << "Could not update status file for snapshot: " << name;
    }
    return true;
}

bool SnapshotManager::RewriteSnapshotDeviceTable(const std::string& dm_name) {
    auto& dm = DeviceMapper::Instance();

    std::vector<DeviceMapper::TargetInfo> old_targets;
    if (!dm.GetTableInfo(dm_name, &old_targets)) {
        LOG(ERROR) << "Could not read snapshot device table: " << dm_name;
        return false;
    }
    if (old_targets.size() != 1 || DeviceMapper::GetTargetType(old_targets[0].spec) != "snapshot") {
        LOG(ERROR) << "Unexpected device-mapper table for snapshot: " << dm_name;
        return false;
    }

    std::string base_device, cow_device;
    if (!DmTargetSnapshot::GetDevicesFromParams(old_targets[0].data, &base_device, &cow_device)) {
        LOG(ERROR) << "Could not derive underlying devices for snapshot: " << dm_name;
        return false;
    }

    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, old_targets[0].spec.length, base_device, cow_device,
                                    SnapshotStorageMode::Merge, kSnapshotChunkSize);
    if (!dm.LoadTableAndActivate(dm_name, table)) {
        LOG(ERROR) << "Could not swap device-mapper tables on snapshot device " << dm_name;
        return false;
    }
    LOG(INFO) << "Successfully switched snapshot device to a merge target: " << dm_name;
    return true;
}

enum class TableQuery {
    Table,
    Status,
};

static bool GetSingleTarget(const std::string& dm_name, TableQuery query,
                            DeviceMapper::TargetInfo* target) {
    auto& dm = DeviceMapper::Instance();
    if (dm.GetState(dm_name) == DmDeviceState::INVALID) {
        return false;
    }

    std::vector<DeviceMapper::TargetInfo> targets;
    bool result;
    if (query == TableQuery::Status) {
        result = dm.GetTableStatus(dm_name, &targets);
    } else {
        result = dm.GetTableInfo(dm_name, &targets);
    }
    if (!result) {
        LOG(ERROR) << "Could not query device: " << dm_name;
        return false;
    }
    if (targets.size() != 1) {
        return false;
    }

    *target = std::move(targets[0]);
    return true;
}

bool SnapshotManager::IsSnapshotDevice(const std::string& dm_name, TargetInfo* target) {
    DeviceMapper::TargetInfo snap_target;
    if (!GetSingleTarget(dm_name, TableQuery::Status, &snap_target)) {
        return false;
    }
    auto type = DeviceMapper::GetTargetType(snap_target.spec);
    if (type != "snapshot" && type != "snapshot-merge") {
        return false;
    }
    if (target) {
        *target = std::move(snap_target);
    }
    return true;
}

bool SnapshotManager::QuerySnapshotStatus(const std::string& dm_name, std::string* target_type,
                                          DmTargetSnapshot::Status* status) {
    DeviceMapper::TargetInfo target;
    if (!IsSnapshotDevice(dm_name, &target)) {
        LOG(ERROR) << "Device " << dm_name << " is not a snapshot or snapshot-merge device";
        return false;
    }
    if (!DmTargetSnapshot::ParseStatusText(target.data, status)) {
        LOG(ERROR) << "Could not parse snapshot status text: " << dm_name;
        return false;
    }
    if (target_type) {
        *target_type = DeviceMapper::GetTargetType(target.spec);
    }
    return true;
}

// Note that when a merge fails, we will *always* try again to complete the
// merge each time the device boots. There is no harm in doing so, and if
// the problem was transient, we might manage to get a new outcome.
UpdateState SnapshotManager::ProcessUpdateState() {
    while (true) {
        UpdateState state = CheckMergeState();
        if (state == UpdateState::MergeFailed) {
            AcknowledgeMergeFailure();
        }
        if (state != UpdateState::Merging) {
            // Either there is no merge, or the merge was finished, so no need
            // to keep waiting.
            return state;
        }

        // This wait is not super time sensitive, so we have a relatively
        // low polling frequency.
        std::this_thread::sleep_for(2s);
    }
}

UpdateState SnapshotManager::CheckMergeState() {
    auto lock = LockExclusive();
    if (!lock) {
        return UpdateState::MergeFailed;
    }

    UpdateState state = CheckMergeState(lock.get());
    if (state == UpdateState::MergeCompleted) {
        // Do this inside the same lock. Failures get acknowledged without the
        // lock, because flock() might have failed.
        AcknowledgeMergeSuccess(lock.get());
    } else if (state == UpdateState::Cancelled) {
        RemoveAllUpdateState(lock.get());
    }
    return state;
}

UpdateState SnapshotManager::CheckMergeState(LockedFile* lock) {
    UpdateState state = ReadUpdateState(lock);
    switch (state) {
        case UpdateState::None:
        case UpdateState::MergeCompleted:
            // Harmless races are allowed between two callers of WaitForMerge,
            // so in both of these cases we just propagate the state.
            return state;

        case UpdateState::Merging:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeFailed:
            // We'll poll each snapshot below. Note that for the NeedsReboot
            // case, we always poll once to give cleanup another opportunity to
            // run.
            break;

        case UpdateState::Unverified:
            // This is an edge case. Normally cancelled updates are detected
            // via the merge poll below, but if we never started a merge, we
            // need to also check here.
            if (HandleCancelledUpdate(lock)) {
                return UpdateState::Cancelled;
            }
            return state;

        default:
            return state;
    }

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return UpdateState::MergeFailed;
    }

    bool cancelled = false;
    bool failed = false;
    bool merging = false;
    bool needs_reboot = false;
    for (const auto& snapshot : snapshots) {
        UpdateState snapshot_state = CheckTargetMergeState(lock, snapshot);
        switch (snapshot_state) {
            case UpdateState::MergeFailed:
                failed = true;
                break;
            case UpdateState::Merging:
                merging = true;
                break;
            case UpdateState::MergeNeedsReboot:
                needs_reboot = true;
                break;
            case UpdateState::MergeCompleted:
                break;
            case UpdateState::Cancelled:
                cancelled = true;
                break;
            default:
                LOG(ERROR) << "Unknown merge status: " << static_cast<uint32_t>(snapshot_state);
                failed = true;
                break;
        }
    }

    if (merging) {
        // Note that we handle "Merging" before we handle anything else. We
        // want to poll until *nothing* is merging if we can, so everything has
        // a chance to get marked as completed or failed.
        return UpdateState::Merging;
    }
    if (failed) {
        // Note: since there are many drop-out cases for failure, we acknowledge
        // it in WaitForMerge rather than here and elsewhere.
        return UpdateState::MergeFailed;
    }
    if (needs_reboot) {
        WriteUpdateState(lock, UpdateState::MergeNeedsReboot);
        return UpdateState::MergeNeedsReboot;
    }
    if (cancelled) {
        // This is an edge case, that we handle as correctly as we sensibly can.
        // The underlying partition has changed behind update_engine, and we've
        // removed the snapshot as a result. The exact state of the update is
        // undefined now, but this can only happen on an unlocked device where
        // partitions can be flashed without wiping userdata.
        return UpdateState::Cancelled;
    }
    return UpdateState::MergeCompleted;
}

UpdateState SnapshotManager::CheckTargetMergeState(LockedFile* lock, const std::string& name) {
    SnapshotStatus snapshot_status;
    if (!ReadSnapshotStatus(lock, name, &snapshot_status)) {
        return UpdateState::MergeFailed;
    }

    std::string dm_name = GetSnapshotDeviceName(name, snapshot_status);

    if (!IsSnapshotDevice(dm_name)) {
        if (IsCancelledSnapshot(name)) {
            DeleteSnapshot(lock, name);
            return UpdateState::Cancelled;
        }

        // During a check, we decided the merge was complete, but we were unable to
        // collapse the device-mapper stack and perform COW cleanup. If we haven't
        // rebooted after this check, the device will still be a snapshot-merge
        // target. If the have rebooted, the device will now be a linear target,
        // and we can try cleanup again.
        if (snapshot_status.state == SnapshotState::MergeCompleted) {
            // NB: It's okay if this fails now, we gave cleanup our best effort.
            OnSnapshotMergeComplete(lock, name, snapshot_status);
            return UpdateState::MergeCompleted;
        }

        LOG(ERROR) << "Expected snapshot or snapshot-merge for device: " << dm_name;
        return UpdateState::MergeFailed;
    }

    // This check is expensive so it is only enabled for debugging.
    DCHECK(!IsCancelledSnapshot(name));

    std::string target_type;
    DmTargetSnapshot::Status status;
    if (!QuerySnapshotStatus(dm_name, &target_type, &status)) {
        return UpdateState::MergeFailed;
    }
    if (target_type != "snapshot-merge") {
        // We can get here if we failed to rewrite the target type in
        // InitiateMerge(). If we failed to create the target in first-stage
        // init, boot would not succeed.
        LOG(ERROR) << "Snapshot " << name << " has incorrect target type: " << target_type;
        return UpdateState::MergeFailed;
    }

    // These two values are equal when merging is complete.
    if (status.sectors_allocated != status.metadata_sectors) {
        if (snapshot_status.state == SnapshotState::MergeCompleted) {
            LOG(ERROR) << "Snapshot " << name << " is merging after being marked merge-complete.";
            return UpdateState::MergeFailed;
        }
        return UpdateState::Merging;
    }

    // Merging is done. First, update the status file to indicate the merge
    // is complete. We do this before calling OnSnapshotMergeComplete, even
    // though this means the write is potentially wasted work (since in the
    // ideal case we'll immediately delete the file).
    //
    // This makes it simpler to reason about the next reboot: no matter what
    // part of cleanup failed, first-stage init won't try to create another
    // snapshot device for this partition.
    snapshot_status.state = SnapshotState::MergeCompleted;
    if (!WriteSnapshotStatus(lock, name, snapshot_status)) {
        return UpdateState::MergeFailed;
    }
    if (!OnSnapshotMergeComplete(lock, name, snapshot_status)) {
        return UpdateState::MergeNeedsReboot;
    }
    return UpdateState::MergeCompleted;
}

std::string SnapshotManager::GetSnapshotBootIndicatorPath() {
    return metadata_dir_ + "/" + android::base::Basename(kBootIndicatorPath);
}

void SnapshotManager::RemoveSnapshotBootIndicator() {
    // It's okay if this fails - first-stage init performs a deeper check after
    // reading the indicator file, so it's not a problem if it still exists
    // after the update completes.
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (unlink(boot_file.c_str()) == -1 && errno != ENOENT) {
        PLOG(ERROR) << "unlink " << boot_file;
    }
}

void SnapshotManager::AcknowledgeMergeSuccess(LockedFile* lock) {
    RemoveAllUpdateState(lock);
}

void SnapshotManager::AcknowledgeMergeFailure() {
    // Log first, so worst case, we always have a record of why the calls below
    // were being made.
    LOG(ERROR) << "Merge could not be completed and will be marked as failed.";

    auto lock = LockExclusive();
    if (!lock) return;

    // Since we released the lock in between WaitForMerge and here, it's
    // possible (1) the merge successfully completed or (2) was already
    // marked as a failure. So make sure to check the state again, and
    // only mark as a failure if appropriate.
    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Merging && state != UpdateState::MergeNeedsReboot) {
        return;
    }

    WriteUpdateState(lock.get(), UpdateState::MergeFailed);
}

bool SnapshotManager::OnSnapshotMergeComplete(LockedFile* lock, const std::string& name,
                                              const SnapshotStatus& status) {
    auto dm_name = GetSnapshotDeviceName(name, status);
    if (IsSnapshotDevice(dm_name)) {
        // We are extra-cautious here, to avoid deleting the wrong table.
        std::string target_type;
        DmTargetSnapshot::Status dm_status;
        if (!QuerySnapshotStatus(dm_name, &target_type, &dm_status)) {
            return false;
        }
        if (target_type != "snapshot-merge") {
            LOG(ERROR) << "Unexpected target type " << target_type
                       << " for snapshot device: " << dm_name;
            return false;
        }
        if (dm_status.sectors_allocated != dm_status.metadata_sectors) {
            LOG(ERROR) << "Merge is unexpectedly incomplete for device " << dm_name;
            return false;
        }
        if (!CollapseSnapshotDevice(name, status)) {
            LOG(ERROR) << "Unable to collapse snapshot: " << name;
            return false;
        }
        // Note that collapsing is implicitly an Unmap, so we don't need to
        // unmap the snapshot.
    }

    if (!DeleteSnapshot(lock, name)) {
        LOG(ERROR) << "Could not delete snapshot: " << name;
        return false;
    }
    return true;
}

bool SnapshotManager::CollapseSnapshotDevice(const std::string& name,
                                             const SnapshotStatus& status) {
    auto& dm = DeviceMapper::Instance();
    auto dm_name = GetSnapshotDeviceName(name, status);

    // Verify we have a snapshot-merge device.
    DeviceMapper::TargetInfo target;
    if (!GetSingleTarget(dm_name, TableQuery::Table, &target)) {
        return false;
    }
    if (DeviceMapper::GetTargetType(target.spec) != "snapshot-merge") {
        // This should be impossible, it was checked earlier.
        LOG(ERROR) << "Snapshot device has invalid target type: " << dm_name;
        return false;
    }

    std::string base_device, cow_device;
    if (!DmTargetSnapshot::GetDevicesFromParams(target.data, &base_device, &cow_device)) {
        LOG(ERROR) << "Could not parse snapshot device " << dm_name
                   << " parameters: " << target.data;
        return false;
    }

    uint64_t snapshot_sectors = status.snapshot_size / kSectorSize;
    if (snapshot_sectors * kSectorSize != status.snapshot_size) {
        LOG(ERROR) << "Snapshot " << name
                   << " size is not sector aligned: " << status.snapshot_size;
        return false;
    }

    if (dm_name != name) {
        // We've derived the base device, but we actually need to replace the
        // table of the outermost device. Do a quick verification that this
        // device looks like we expect it to.
        std::vector<DeviceMapper::TargetInfo> outer_table;
        if (!dm.GetTableInfo(name, &outer_table)) {
            LOG(ERROR) << "Could not validate outer snapshot table: " << name;
            return false;
        }
        if (outer_table.size() != 2) {
            LOG(ERROR) << "Expected 2 dm-linear targets for table " << name
                       << ", got: " << outer_table.size();
            return false;
        }
        for (const auto& target : outer_table) {
            auto target_type = DeviceMapper::GetTargetType(target.spec);
            if (target_type != "linear") {
                LOG(ERROR) << "Outer snapshot table may only contain linear targets, but " << name
                           << " has target: " << target_type;
                return false;
            }
        }
        if (outer_table[0].spec.length != snapshot_sectors) {
            LOG(ERROR) << "dm-snapshot " << name << " should have " << snapshot_sectors
                       << " sectors, got: " << outer_table[0].spec.length;
            return false;
        }
        uint64_t expected_device_sectors = status.device_size / kSectorSize;
        uint64_t actual_device_sectors = outer_table[0].spec.length + outer_table[1].spec.length;
        if (expected_device_sectors != actual_device_sectors) {
            LOG(ERROR) << "Outer device " << name << " should have " << expected_device_sectors
                       << " sectors, got: " << actual_device_sectors;
            return false;
        }
    }

    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    // Create a DmTable that is identical to the base device.
    CreateLogicalPartitionParams base_device_params{
            .block_device = device_->GetSuperDevice(slot),
            .metadata_slot = slot,
            .partition_name = name,
            .partition_opener = &device_->GetPartitionOpener(),
    };
    DmTable table;
    if (!CreateDmTable(base_device_params, &table)) {
        LOG(ERROR) << "Could not create a DmTable for partition: " << name;
        return false;
    }

    // Note: we are replacing the *outer* table here, so we do not use dm_name.
    if (!dm.LoadTableAndActivate(name, table)) {
        return false;
    }

    // Attempt to delete the snapshot device if one still exists. Nothing
    // should be depending on the device, and device-mapper should have
    // flushed remaining I/O. We could in theory replace with dm-zero (or
    // re-use the table above), but for now it's better to know why this
    // would fail.
    if (dm_name != name && !dm.DeleteDeviceIfExists(dm_name)) {
        LOG(ERROR) << "Unable to delete snapshot device " << dm_name << ", COW cannot be "
                   << "reclaimed until after reboot.";
        return false;
    }

    // Cleanup the base device as well, since it is no longer used. This does
    // not block cleanup.
    auto base_name = GetBaseDeviceName(name);
    if (!dm.DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Unable to delete base device for snapshot: " << base_name;
    }
    return true;
}

bool SnapshotManager::HandleCancelledUpdate(LockedFile* lock) {
    std::string old_slot;
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (!android::base::ReadFileToString(boot_file, &old_slot)) {
        PLOG(ERROR) << "Unable to read the snapshot indicator file: " << boot_file;
        return false;
    }
    if (device_->GetSlotSuffix() != old_slot) {
        // We're booted into the target slot, which means we just rebooted
        // after applying the update.
        return false;
    }

    // The only way we can get here is if:
    //  (1) The device rolled back to the previous slot.
    //  (2) This function was called prematurely before rebooting the device.
    //  (3) fastboot set_active was used.
    //
    // In any case, delete the snapshots. It may be worth using the boot_control
    // HAL to differentiate case (2).
    RemoveAllUpdateState(lock);
    return true;
}

bool SnapshotManager::IsCancelledSnapshot(const std::string& snapshot_name) {
    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_device = device_->GetSuperDevice(slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return false;
    }
    auto partition = android::fs_mgr::FindPartition(*metadata.get(), snapshot_name);
    if (!partition) return false;
    return (partition->attributes & LP_PARTITION_ATTR_UPDATED) == 0;
}

bool SnapshotManager::RemoveAllSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return false;
    }

    bool ok = true;
    for (const auto& name : snapshots) {
        ok &= (UnmapPartitionWithSnapshot(lock, name) && DeleteSnapshot(lock, name));
    }
    return ok;
}

UpdateState SnapshotManager::GetUpdateState(double* progress) {
    // If we've never started an update, the state file won't exist.
    auto state_file = GetStateFilePath();
    if (access(state_file.c_str(), F_OK) != 0 && errno == ENOENT) {
        return UpdateState::None;
    }

    auto file = LockShared();
    if (!file) {
        return UpdateState::None;
    }

    auto state = ReadUpdateState(file.get());
    if (progress) {
        *progress = 0.0;
        if (state == UpdateState::Merging) {
            // :TODO: When merging is implemented, set progress_val.
        } else if (state == UpdateState::MergeCompleted) {
            *progress = 100.0;
        }
    }
    return state;
}

bool SnapshotManager::ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots) {
    CHECK(lock);

    auto dir_path = metadata_dir_ + "/snapshots"s;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(dir_path.c_str()), closedir);
    if (!dir) {
        PLOG(ERROR) << "opendir failed: " << dir_path;
        return false;
    }

    struct dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        if (dp->d_type != DT_REG) continue;
        snapshots->emplace_back(dp->d_name);
    }
    return true;
}

bool SnapshotManager::IsSnapshotManagerNeeded() {
    return access(kBootIndicatorPath, F_OK) == 0;
}

bool SnapshotManager::NeedSnapshotsInFirstStageMount() {
    // If we fail to read, we'll wind up using CreateLogicalPartitions, which
    // will create devices that look like the old slot, except with extra
    // content at the end of each device. This will confuse dm-verity, and
    // ultimately we'll fail to boot. Why not make it a fatal error and have
    // the reason be clearer? Because the indicator file still exists, and
    // if this was FATAL, reverting to the old slot would be broken.
    std::string old_slot;
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (!android::base::ReadFileToString(boot_file, &old_slot)) {
        PLOG(ERROR) << "Unable to read the snapshot indicator file: " << boot_file;
        return false;
    }
    if (device_->GetSlotSuffix() == old_slot) {
        LOG(INFO) << "Detected slot rollback, will not mount snapshots.";
        return false;
    }

    // If we can't read the update state, it's unlikely anything else will
    // succeed, so this is a fatal error. We'll eventually exhaust boot
    // attempts and revert to the old slot.
    auto lock = LockShared();
    if (!lock) {
        LOG(FATAL) << "Could not read update state to determine snapshot status";
        return false;
    }
    switch (ReadUpdateState(lock.get())) {
        case UpdateState::Unverified:
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            return true;
        default:
            return false;
    }
}

bool SnapshotManager::CreateLogicalAndSnapshotPartitions(const std::string& super_device) {
    LOG(INFO) << "Creating logical partitions with snapshots as needed";

    auto lock = LockExclusive();
    if (!lock) return false;

    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return false;
    }

    for (const auto& partition : metadata->partitions) {
        if (GetPartitionGroupName(metadata->groups[partition.group_index]) == kCowGroupName) {
            LOG(INFO) << "Skip mapping partition " << GetPartitionName(partition) << " in group "
                      << kCowGroupName;
            continue;
        }

        CreateLogicalPartitionParams params = {
                .block_device = super_device,
                .metadata = metadata.get(),
                .partition = &partition,
                .partition_opener = &opener,
        };
        std::string ignore_path;
        if (!MapPartitionWithSnapshot(lock.get(), std::move(params), &ignore_path)) {
            return false;
        }
    }

    LOG(INFO) << "Created logical partitions with snapshot.";
    return true;
}

static std::chrono::milliseconds GetRemainingTime(
        const std::chrono::milliseconds& timeout,
        const std::chrono::time_point<std::chrono::steady_clock>& begin) {
    // If no timeout is specified, execute all commands without specifying any timeout.
    if (timeout.count() == 0) return std::chrono::milliseconds(0);
    auto passed_time = std::chrono::steady_clock::now() - begin;
    auto remaining_time = timeout - duration_cast<std::chrono::milliseconds>(passed_time);
    if (remaining_time.count() <= 0) {
        LOG(ERROR) << "MapPartitionWithSnapshot has reached timeout " << timeout.count() << "ms ("
                   << remaining_time.count() << "ms remaining)";
        // Return min() instead of remaining_time here because 0 is treated as a special value for
        // no timeout, where the rest of the commands will still be executed.
        return std::chrono::milliseconds::min();
    }
    return remaining_time;
}

bool SnapshotManager::MapPartitionWithSnapshot(LockedFile* lock,
                                               CreateLogicalPartitionParams params,
                                               std::string* path) {
    auto begin = std::chrono::steady_clock::now();

    CHECK(lock);
    path->clear();

    if (params.GetPartitionName() != params.GetDeviceName()) {
        LOG(ERROR) << "Mapping snapshot with a different name is unsupported: partition_name = "
                   << params.GetPartitionName() << ", device_name = " << params.GetDeviceName();
        return false;
    }

    // Fill out fields in CreateLogicalPartitionParams so that we have more information (e.g. by
    // reading super partition metadata).
    CreateLogicalPartitionParams::OwnedData params_owned_data;
    if (!params.InitDefaults(&params_owned_data)) {
        return false;
    }

    if (!params.partition->num_extents) {
        LOG(INFO) << "Skipping zero-length logical partition: " << params.GetPartitionName();
        return true;  // leave path empty to indicate that nothing is mapped.
    }

    // Determine if there is a live snapshot for the SnapshotStatus of the partition; i.e. if the
    // partition still has a snapshot that needs to be mapped.  If no live snapshot or merge
    // completed, live_snapshot_status is set to nullopt.
    std::optional<SnapshotStatus> live_snapshot_status;
    do {
        if (!(params.partition->attributes & LP_PARTITION_ATTR_UPDATED)) {
            LOG(INFO) << "Detected re-flashing of partition, will skip snapshot: "
                      << params.GetPartitionName();
            break;
        }
        auto file_path = GetSnapshotStatusFilePath(params.GetPartitionName());
        if (access(file_path.c_str(), F_OK) != 0) {
            if (errno != ENOENT) {
                PLOG(INFO) << "Can't map snapshot for " << params.GetPartitionName()
                           << ": Can't access " << file_path;
                return false;
            }
            break;
        }
        live_snapshot_status = std::make_optional<SnapshotStatus>();
        if (!ReadSnapshotStatus(lock, params.GetPartitionName(), &*live_snapshot_status)) {
            return false;
        }
        // No live snapshot if merge is completed.
        if (live_snapshot_status->state == SnapshotState::MergeCompleted) {
            live_snapshot_status.reset();
        }
    } while (0);

    if (live_snapshot_status.has_value()) {
        // dm-snapshot requires the base device to be writable.
        params.force_writable = true;
        // Map the base device with a different name to avoid collision.
        params.device_name = GetBaseDeviceName(params.GetPartitionName());
    }

    AutoDeviceList created_devices;

    // Create the base device for the snapshot, or if there is no snapshot, the
    // device itself. This device consists of the real blocks in the super
    // partition that this logical partition occupies.
    auto& dm = DeviceMapper::Instance();
    std::string ignore_path;
    if (!CreateLogicalPartition(params, &ignore_path)) {
        LOG(ERROR) << "Could not create logical partition " << params.GetPartitionName()
                   << " as device " << params.GetDeviceName();
        return false;
    }
    created_devices.EmplaceBack<AutoUnmapDevice>(&dm, params.GetDeviceName());

    if (!live_snapshot_status.has_value()) {
        created_devices.Release();
        return true;
    }

    // We don't have ueventd in first-stage init, so use device major:minor
    // strings instead.
    std::string base_device;
    if (!dm.GetDeviceString(params.GetDeviceName(), &base_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << params.GetDeviceName();
        return false;
    }

    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;

    std::string cow_name;
    CreateLogicalPartitionParams cow_params = params;
    cow_params.timeout_ms = remaining_time;
    if (!MapCowDevices(lock, cow_params, *live_snapshot_status, &created_devices, &cow_name)) {
        return false;
    }
    std::string cow_device;
    if (!dm.GetDeviceString(cow_name, &cow_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << cow_name;
        return false;
    }

    remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;

    if (!MapSnapshot(lock, params.GetPartitionName(), base_device, cow_device, remaining_time,
                     path)) {
        LOG(ERROR) << "Could not map snapshot for partition: " << params.GetPartitionName();
        return false;
    }
    // No need to add params.GetPartitionName() to created_devices since it is immediately released.

    created_devices.Release();

    LOG(INFO) << "Mapped " << params.GetPartitionName() << " as snapshot device at " << *path;

    return true;
}

bool SnapshotManager::UnmapPartitionWithSnapshot(LockedFile* lock,
                                                 const std::string& target_partition_name) {
    CHECK(lock);

    if (!UnmapSnapshot(lock, target_partition_name)) {
        return false;
    }

    if (!UnmapCowDevices(lock, target_partition_name)) {
        return false;
    }

    auto& dm = DeviceMapper::Instance();
    std::string base_name = GetBaseDeviceName(target_partition_name);
    if (!dm.DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Cannot delete base device: " << base_name;
        return false;
    }

    LOG(INFO) << "Successfully unmapped snapshot " << target_partition_name;

    return true;
}

bool SnapshotManager::MapCowDevices(LockedFile* lock, const CreateLogicalPartitionParams& params,
                                    const SnapshotStatus& snapshot_status,
                                    AutoDeviceList* created_devices, std::string* cow_name) {
    CHECK(lock);
    if (!EnsureImageManager()) return false;
    CHECK(snapshot_status.cow_partition_size + snapshot_status.cow_file_size > 0);
    auto begin = std::chrono::steady_clock::now();

    std::string partition_name = params.GetPartitionName();
    std::string cow_image_name = GetCowImageDeviceName(partition_name);
    *cow_name = GetCowName(partition_name);

    auto& dm = DeviceMapper::Instance();

    // Map COW image if necessary.
    if (snapshot_status.cow_file_size > 0) {
        auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
        if (remaining_time.count() < 0) return false;

        if (!MapCowImage(partition_name, remaining_time)) {
            LOG(ERROR) << "Could not map cow image for partition: " << partition_name;
            return false;
        }
        created_devices->EmplaceBack<AutoUnmapImage>(images_.get(), cow_image_name);

        // If no COW partition exists, just return the image alone.
        if (snapshot_status.cow_partition_size == 0) {
            *cow_name = std::move(cow_image_name);
            LOG(INFO) << "Mapped COW image for " << partition_name << " at " << *cow_name;
            return true;
        }
    }

    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;

    CHECK(snapshot_status.cow_partition_size > 0);

    // Create the DmTable for the COW device. It is the DmTable of the COW partition plus
    // COW image device as the last extent.
    CreateLogicalPartitionParams cow_partition_params = params;
    cow_partition_params.partition = nullptr;
    cow_partition_params.partition_name = *cow_name;
    cow_partition_params.device_name.clear();
    DmTable table;
    if (!CreateDmTable(cow_partition_params, &table)) {
        return false;
    }
    // If the COW image exists, append it as the last extent.
    if (snapshot_status.cow_file_size > 0) {
        std::string cow_image_device;
        if (!dm.GetDeviceString(cow_image_name, &cow_image_device)) {
            LOG(ERROR) << "Cannot determine major/minor for: " << cow_image_name;
            return false;
        }
        auto cow_partition_sectors = snapshot_status.cow_partition_size / kSectorSize;
        auto cow_image_sectors = snapshot_status.cow_file_size / kSectorSize;
        table.Emplace<DmTargetLinear>(cow_partition_sectors, cow_image_sectors, cow_image_device,
                                      0);
    }

    // We have created the DmTable now. Map it.
    std::string cow_path;
    if (!dm.CreateDevice(*cow_name, table, &cow_path, remaining_time)) {
        LOG(ERROR) << "Could not create COW device: " << *cow_name;
        return false;
    }
    created_devices->EmplaceBack<AutoUnmapDevice>(&dm, *cow_name);
    LOG(INFO) << "Mapped COW device for " << params.GetPartitionName() << " at " << cow_path;
    return true;
}

bool SnapshotManager::UnmapCowDevices(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    if (!EnsureImageManager()) return false;

    auto& dm = DeviceMapper::Instance();
    auto cow_name = GetCowName(name);
    if (!dm.DeleteDeviceIfExists(cow_name)) {
        LOG(ERROR) << "Cannot unmap " << cow_name;
        return false;
    }

    std::string cow_image_name = GetCowImageDeviceName(name);
    if (!images_->UnmapImageIfExists(cow_image_name)) {
        LOG(ERROR) << "Cannot unmap image " << cow_image_name;
        return false;
    }
    return true;
}

auto SnapshotManager::OpenFile(const std::string& file, int open_flags, int lock_flags)
        -> std::unique_ptr<LockedFile> {
    unique_fd fd(open(file.c_str(), open_flags | O_CLOEXEC | O_NOFOLLOW | O_SYNC, 0660));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << file;
        return nullptr;
    }
    if (lock_flags != 0 && flock(fd, lock_flags) < 0) {
        PLOG(ERROR) << "Acquire flock failed: " << file;
        return nullptr;
    }
    // For simplicity, we want to CHECK that lock_mode == LOCK_EX, in some
    // calls, so strip extra flags.
    int lock_mode = lock_flags & (LOCK_EX | LOCK_SH);
    return std::make_unique<LockedFile>(file, std::move(fd), lock_mode);
}

SnapshotManager::LockedFile::~LockedFile() {
    if (flock(fd_, LOCK_UN) < 0) {
        PLOG(ERROR) << "Failed to unlock file: " << path_;
    }
}

std::string SnapshotManager::GetStateFilePath() const {
    return metadata_dir_ + "/state"s;
}

std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::OpenStateFile(int open_flags,
                                                                            int lock_flags) {
    auto state_file = GetStateFilePath();
    return OpenFile(state_file, open_flags, lock_flags);
}

std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::LockShared() {
    return OpenStateFile(O_RDONLY, LOCK_SH);
}

std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::LockExclusive() {
    return OpenStateFile(O_RDWR | O_CREAT, LOCK_EX);
}

UpdateState SnapshotManager::ReadUpdateState(LockedFile* file) {
    // Reset position since some calls read+write.
    if (lseek(file->fd(), 0, SEEK_SET) < 0) {
        PLOG(ERROR) << "lseek state file failed";
        return UpdateState::None;
    }

    std::string contents;
    if (!android::base::ReadFdToString(file->fd(), &contents)) {
        PLOG(ERROR) << "Read state file failed";
        return UpdateState::None;
    }

    if (contents.empty() || contents == "none") {
        return UpdateState::None;
    } else if (contents == "initiated") {
        return UpdateState::Initiated;
    } else if (contents == "unverified") {
        return UpdateState::Unverified;
    } else if (contents == "merging") {
        return UpdateState::Merging;
    } else if (contents == "merge-completed") {
        return UpdateState::MergeCompleted;
    } else if (contents == "merge-needs-reboot") {
        return UpdateState::MergeNeedsReboot;
    } else if (contents == "merge-failed") {
        return UpdateState::MergeFailed;
    } else {
        LOG(ERROR) << "Unknown merge state in update state file";
        return UpdateState::None;
    }
}

std::ostream& operator<<(std::ostream& os, UpdateState state) {
    switch (state) {
        case UpdateState::None:
            return os << "none";
        case UpdateState::Initiated:
            return os << "initiated";
        case UpdateState::Unverified:
            return os << "unverified";
        case UpdateState::Merging:
            return os << "merging";
        case UpdateState::MergeCompleted:
            return os << "merge-completed";
        case UpdateState::MergeNeedsReboot:
            return os << "merge-needs-reboot";
        case UpdateState::MergeFailed:
            return os << "merge-failed";
        default:
            LOG(ERROR) << "Unknown update state";
            return os;
    }
}

bool SnapshotManager::WriteUpdateState(LockedFile* file, UpdateState state) {
    std::stringstream ss;
    ss << state;
    std::string contents = ss.str();
    if (contents.empty()) return false;

    if (!Truncate(file)) return false;
    if (!android::base::WriteStringToFd(contents, file->fd())) {
        PLOG(ERROR) << "Could not write to state file";
        return false;
    }
    return true;
}

std::string SnapshotManager::GetSnapshotStatusFilePath(const std::string& name) {
    auto file = metadata_dir_ + "/snapshots/"s + name;
    return file;
}

bool SnapshotManager::ReadSnapshotStatus(LockedFile* lock, const std::string& name,
                                         SnapshotStatus* status) {
    CHECK(lock);
    auto path = GetSnapshotStatusFilePath(name);

    unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << path;
        return false;
    }

    std::string contents;
    if (!android::base::ReadFdToString(fd, &contents)) {
        PLOG(ERROR) << "read failed: " << path;
        return false;
    }
    auto pieces = android::base::Split(contents, " ");
    if (pieces.size() != 7) {
        LOG(ERROR) << "Invalid status line for snapshot: " << path;
        return false;
    }

    if (pieces[0] == "none") {
        status->state = SnapshotState::None;
    } else if (pieces[0] == "created") {
        status->state = SnapshotState::Created;
    } else if (pieces[0] == "merging") {
        status->state = SnapshotState::Merging;
    } else if (pieces[0] == "merge-completed") {
        status->state = SnapshotState::MergeCompleted;
    } else {
        LOG(ERROR) << "Unrecognized state " << pieces[0] << " for snapshot: " << name;
        return false;
    }

    if (!android::base::ParseUint(pieces[1], &status->device_size)) {
        LOG(ERROR) << "Invalid device size in status line for: " << path;
        return false;
    }
    if (!android::base::ParseUint(pieces[2], &status->snapshot_size)) {
        LOG(ERROR) << "Invalid snapshot size in status line for: " << path;
        return false;
    }
    if (!android::base::ParseUint(pieces[3], &status->cow_partition_size)) {
        LOG(ERROR) << "Invalid cow linear size in status line for: " << path;
        return false;
    }
    if (!android::base::ParseUint(pieces[4], &status->cow_file_size)) {
        LOG(ERROR) << "Invalid cow file size in status line for: " << path;
        return false;
    }
    if (!android::base::ParseUint(pieces[5], &status->sectors_allocated)) {
        LOG(ERROR) << "Invalid snapshot size in status line for: " << path;
        return false;
    }
    if (!android::base::ParseUint(pieces[6], &status->metadata_sectors)) {
        LOG(ERROR) << "Invalid snapshot size in status line for: " << path;
        return false;
    }
    return true;
}

std::string SnapshotManager::to_string(SnapshotState state) {
    switch (state) {
        case SnapshotState::None:
            return "none";
        case SnapshotState::Created:
            return "created";
        case SnapshotState::Merging:
            return "merging";
        case SnapshotState::MergeCompleted:
            return "merge-completed";
        default:
            LOG(ERROR) << "Unknown snapshot state: " << (int)state;
            return "unknown";
    }
}

bool SnapshotManager::WriteSnapshotStatus(LockedFile* lock, const std::string& name,
                                          const SnapshotStatus& status) {
    // The caller must take an exclusive lock to modify snapshots.
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);

    auto path = GetSnapshotStatusFilePath(name);
    unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_SYNC, 0660));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << path;
        return false;
    }

    std::vector<std::string> pieces = {
            to_string(status.state),
            std::to_string(status.device_size),
            std::to_string(status.snapshot_size),
            std::to_string(status.cow_partition_size),
            std::to_string(status.cow_file_size),
            std::to_string(status.sectors_allocated),
            std::to_string(status.metadata_sectors),
    };
    auto contents = android::base::Join(pieces, " ");

    if (!android::base::WriteStringToFd(contents, fd)) {
        PLOG(ERROR) << "write failed: " << path;
        return false;
    }
    return true;
}

bool SnapshotManager::Truncate(LockedFile* file) {
    if (lseek(file->fd(), 0, SEEK_SET) < 0) {
        PLOG(ERROR) << "lseek file failed: " << file->path();
        return false;
    }
    if (ftruncate(file->fd(), 0) < 0) {
        PLOG(ERROR) << "truncate failed: " << file->path();
        return false;
    }
    return true;
}

std::string SnapshotManager::GetSnapshotDeviceName(const std::string& snapshot_name,
                                                   const SnapshotStatus& status) {
    if (status.device_size != status.snapshot_size) {
        return GetSnapshotExtraDeviceName(snapshot_name);
    }
    return snapshot_name;
}

bool SnapshotManager::EnsureImageManager() {
    if (images_) return true;

    // For now, use a preset timeout.
    images_ = android::fiemap::IImageManager::Open(gsid_dir_, 15000ms);
    if (!images_) {
        LOG(ERROR) << "Could not open ImageManager";
        return false;
    }
    return true;
}

bool SnapshotManager::ForceLocalImageManager() {
    images_ = android::fiemap::ImageManager::Open(gsid_dir_);
    if (!images_) {
        LOG(ERROR) << "Could not open ImageManager";
        return false;
    }
    has_local_image_manager_ = true;
    return true;
}

bool SnapshotManager::CreateUpdateSnapshots(const DeltaArchiveManifest& manifest) {
    auto lock = LockExclusive();
    if (!lock) return false;

    const auto& opener = device_->GetPartitionOpener();
    auto current_suffix = device_->GetSlotSuffix();
    uint32_t current_slot = SlotNumberForSlotSuffix(current_suffix);
    auto target_suffix = device_->GetOtherSlotSuffix();
    uint32_t target_slot = SlotNumberForSlotSuffix(target_suffix);
    auto current_super = device_->GetSuperDevice(current_slot);

    auto current_metadata = MetadataBuilder::New(opener, current_super, current_slot);
    auto target_metadata =
            MetadataBuilder::NewForUpdate(opener, current_super, current_slot, target_slot);

    SnapshotMetadataUpdater metadata_updater(target_metadata.get(), target_slot, manifest);
    if (!metadata_updater.Update()) {
        LOG(ERROR) << "Cannot calculate new metadata.";
        return false;
    }

    if (!target_metadata->AddGroup(kCowGroupName, 0)) {
        LOG(ERROR) << "Cannot add group " << kCowGroupName;
        return false;
    }

    std::map<std::string, const RepeatedPtrField<InstallOperation>*> install_operation_map;
    for (const auto& partition_update : manifest.partitions()) {
        auto suffixed_name = partition_update.partition_name() + target_suffix;
        auto&& [it, inserted] = install_operation_map.emplace(std::move(suffixed_name),
                                                              &partition_update.operations());
        if (!inserted) {
            LOG(ERROR) << "Duplicated partition " << partition_update.partition_name()
                       << " in update manifest.";
            return false;
        }
    }

    // TODO(b/134949511): remove this check. Right now, with overlayfs mounted, the scratch
    // partition takes up a big chunk of space in super, causing COW images to be created on
    // retrofit Virtual A/B devices.
    if (device_->IsOverlayfsSetup()) {
        LOG(ERROR) << "Cannot create update snapshots with overlayfs setup. Run `adb enable-verity`"
                   << ", reboot, then try again.";
        return false;
    }

    // Check that all these metadata is not retrofit dynamic partitions. Snapshots on
    // devices with retrofit dynamic partitions does not make sense.
    // This ensures that current_metadata->GetFreeRegions() uses the same device
    // indices as target_metadata (i.e. 0 -> "super").
    // This is also assumed in MapCowDevices() call below.
    CHECK(current_metadata->GetBlockDevicePartitionName(0) == LP_METADATA_DEFAULT_PARTITION_NAME &&
          target_metadata->GetBlockDevicePartitionName(0) == LP_METADATA_DEFAULT_PARTITION_NAME);

    std::map<std::string, SnapshotStatus> all_snapshot_status;

    // In case of error, automatically delete devices that are created along the way.
    // Note that "lock" is destroyed after "created_devices", so it is safe to use |lock| for
    // these devices.
    AutoDeviceList created_devices;

    for (auto* target_partition : ListPartitionsWithSuffix(target_metadata.get(), target_suffix)) {
        const RepeatedPtrField<InstallOperation>* operations = nullptr;
        auto operations_it = install_operation_map.find(target_partition->name());
        if (operations_it != install_operation_map.end()) {
            operations = operations_it->second;
        }

        // Compute the device sizes for the partition.
        PartitionCowCreator cow_creator{target_metadata.get(),  target_suffix,  target_partition,
                                        current_metadata.get(), current_suffix, operations};
        auto cow_creator_ret = cow_creator.Run();
        if (!cow_creator_ret.has_value()) {
            return false;
        }

        LOG(INFO) << "For partition " << target_partition->name()
                  << ", device size = " << cow_creator_ret->snapshot_status.device_size
                  << ", snapshot size = " << cow_creator_ret->snapshot_status.snapshot_size
                  << ", cow partition size = "
                  << cow_creator_ret->snapshot_status.cow_partition_size
                  << ", cow file size = " << cow_creator_ret->snapshot_status.cow_file_size;

        // Delete any existing snapshot before re-creating one.
        if (!DeleteSnapshot(lock.get(), target_partition->name())) {
            LOG(ERROR) << "Cannot delete existing snapshot before creating a new one for partition "
                       << target_partition->name();
            return false;
        }

        // It is possible that the whole partition uses free space in super, and snapshot / COW
        // would not be needed. In this case, skip the partition.
        bool needs_snapshot = cow_creator_ret->snapshot_status.snapshot_size > 0;
        bool needs_cow = (cow_creator_ret->snapshot_status.cow_partition_size +
                          cow_creator_ret->snapshot_status.cow_file_size) > 0;
        CHECK(needs_snapshot == needs_cow);

        if (!needs_snapshot) {
            LOG(INFO) << "Skip creating snapshot for partition " << target_partition->name()
                      << "because nothing needs to be snapshotted.";
            continue;
        }

        // Store these device sizes to snapshot status file.
        if (!CreateSnapshot(lock.get(), target_partition->name(),
                            cow_creator_ret->snapshot_status)) {
            return false;
        }
        created_devices.EmplaceBack<AutoDeleteSnapshot>(this, lock.get(), target_partition->name());

        // Create the COW partition. That is, use any remaining free space in super partition before
        // creating the COW images.
        if (cow_creator_ret->snapshot_status.cow_partition_size > 0) {
            CHECK(cow_creator_ret->snapshot_status.cow_partition_size % kSectorSize == 0)
                    << "cow_partition_size == "
                    << cow_creator_ret->snapshot_status.cow_partition_size
                    << " is not a multiple of sector size " << kSectorSize;
            auto cow_partition = target_metadata->AddPartition(GetCowName(target_partition->name()),
                                                               kCowGroupName, 0 /* flags */);
            if (cow_partition == nullptr) {
                return false;
            }

            if (!target_metadata->ResizePartition(
                        cow_partition, cow_creator_ret->snapshot_status.cow_partition_size,
                        cow_creator_ret->cow_partition_usable_regions)) {
                LOG(ERROR) << "Cannot create COW partition on metadata with size "
                           << cow_creator_ret->snapshot_status.cow_partition_size;
                return false;
            }
            // Only the in-memory target_metadata is modified; nothing to clean up if there is an
            // error in the future.
        }

        // Create the backing COW image if necessary.
        if (cow_creator_ret->snapshot_status.cow_file_size > 0) {
            if (!CreateCowImage(lock.get(), target_partition->name())) {
                return false;
            }
        }

        all_snapshot_status[target_partition->name()] = std::move(cow_creator_ret->snapshot_status);

        LOG(INFO) << "Successfully created snapshot for " << target_partition->name();
    }

    auto& dm = DeviceMapper::Instance();
    auto exported_target_metadata = target_metadata->Export();
    if (exported_target_metadata == nullptr) {
        LOG(ERROR) << "Cannot export target metadata";
        return false;
    }
    CreateLogicalPartitionParams cow_params{
            .block_device = LP_METADATA_DEFAULT_PARTITION_NAME,
            .metadata = exported_target_metadata.get(),
            .timeout_ms = std::chrono::milliseconds::max(),
            .partition_opener = &device_->GetPartitionOpener(),
    };
    for (auto* target_partition : ListPartitionsWithSuffix(target_metadata.get(), target_suffix)) {
        AutoDeviceList created_devices_for_cow;

        if (!UnmapPartitionWithSnapshot(lock.get(), target_partition->name())) {
            LOG(ERROR) << "Cannot unmap existing COW devices before re-mapping them for zero-fill: "
                       << target_partition->name();
            return false;
        }

        auto it = all_snapshot_status.find(target_partition->name());
        CHECK(it != all_snapshot_status.end()) << target_partition->name();
        cow_params.partition_name = target_partition->name();
        std::string cow_name;
        if (!MapCowDevices(lock.get(), cow_params, it->second, &created_devices_for_cow,
                           &cow_name)) {
            return false;
        }

        std::string cow_path;
        if (!dm.GetDmDevicePathByName(cow_name, &cow_path)) {
            LOG(ERROR) << "Cannot determine path for " << cow_name;
            return false;
        }

        if (!InitializeCow(cow_path)) {
            LOG(ERROR) << "Can't zero-fill COW device for " << target_partition->name() << ": "
                       << cow_path;
            return false;
        }
        // Let destructor of created_devices_for_cow to unmap the COW devices.
    };

    if (!UpdatePartitionTable(opener, device_->GetSuperDevice(target_slot),
                              *exported_target_metadata, target_slot)) {
        LOG(ERROR) << "Cannot write target metadata";
        return false;
    }

    created_devices.Release();
    LOG(INFO) << "Successfully created all snapshots for target slot " << target_suffix;

    return true;
}

bool SnapshotManager::MapUpdateSnapshot(const CreateLogicalPartitionParams& params,
                                        std::string* snapshot_path) {
    auto lock = LockShared();
    if (!lock) return false;
    if (!UnmapPartitionWithSnapshot(lock.get(), params.GetPartitionName())) {
        LOG(ERROR) << "Cannot unmap existing snapshot before re-mapping it: "
                   << params.GetPartitionName();
        return false;
    }
    return MapPartitionWithSnapshot(lock.get(), params, snapshot_path);
}

bool SnapshotManager::UnmapUpdateSnapshot(const std::string& target_partition_name) {
    auto lock = LockShared();
    if (!lock) return false;
    return UnmapPartitionWithSnapshot(lock.get(), target_partition_name);
}

bool SnapshotManager::Dump(std::ostream& os) {
    // Don't actually lock. Dump() is for debugging purposes only, so it is okay
    // if it is racy.
    auto file = OpenStateFile(O_RDONLY, 0);
    if (!file) return false;

    std::stringstream ss;

    ss << "Update state: " << ReadUpdateState(file.get()) << std::endl;

    auto boot_file = GetSnapshotBootIndicatorPath();
    std::string boot_indicator;
    if (android::base::ReadFileToString(boot_file, &boot_indicator)) {
        ss << "Boot indicator: old slot = " << boot_indicator << std::endl;
    }

    bool ok = true;
    std::vector<std::string> snapshots;
    if (!ListSnapshots(file.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        snapshots.clear();
        ok = false;
    }
    for (const auto& name : snapshots) {
        ss << "Snapshot: " << name << std::endl;
        SnapshotStatus status;
        if (!ReadSnapshotStatus(file.get(), name, &status)) {
            ok = false;
            continue;
        }
        ss << "    state: " << to_string(status.state) << std::endl;
        ss << "    device size (bytes): " << status.device_size << std::endl;
        ss << "    snapshot size (bytes): " << status.snapshot_size << std::endl;
        ss << "    cow partition size (bytes): " << status.cow_partition_size << std::endl;
        ss << "    cow file size (bytes): " << status.cow_file_size << std::endl;
        ss << "    allocated sectors: " << status.sectors_allocated << std::endl;
        ss << "    metadata sectors: " << status.metadata_sectors << std::endl;
    }
    os << ss.rdbuf();
    return ok;
}

}  // namespace snapshot
}  // namespace android
