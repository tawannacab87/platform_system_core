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

#pragma once

#include <stdint.h>

#include <chrono>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <android-base/unique_fd.h>
#include <fs_mgr_dm_linear.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/builder.h>
#include <liblp/liblp.h>
#include <update_engine/update_metadata.pb.h>

#ifndef FRIEND_TEST
#define FRIEND_TEST(test_set_name, individual_test) \
    friend class test_set_name##_##individual_test##_Test
#define DEFINED_FRIEND_TEST
#endif

namespace android {

namespace fiemap {
class IImageManager;
}  // namespace fiemap

namespace fs_mgr {
struct CreateLogicalPartitionParams;
class IPartitionOpener;
}  // namespace fs_mgr

namespace snapshot {

struct AutoDeleteCowImage;
struct AutoDeleteSnapshot;
struct PartitionCowCreator;
struct AutoDeviceList;

static constexpr const std::string_view kCowGroupName = "cow";

enum class UpdateState : unsigned int {
    // No update or merge is in progress.
    None,

    // An update is applying; snapshots may already exist.
    Initiated,

    // An update is pending, but has not been successfully booted yet.
    Unverified,

    // The kernel is merging in the background.
    Merging,

    // Post-merge cleanup steps could not be completed due to a transient
    // error, but the next reboot will finish any pending operations.
    MergeNeedsReboot,

    // Merging is complete, and needs to be acknowledged.
    MergeCompleted,

    // Merging failed due to an unrecoverable error.
    MergeFailed,

    // The update was implicitly cancelled, either by a rollback or a flash
    // operation via fastboot. This state can only be returned by WaitForMerge.
    Cancelled
};
std::ostream& operator<<(std::ostream& os, UpdateState state);

class SnapshotManager final {
    using CreateLogicalPartitionParams = android::fs_mgr::CreateLogicalPartitionParams;
    using IPartitionOpener = android::fs_mgr::IPartitionOpener;
    using LpMetadata = android::fs_mgr::LpMetadata;
    using MetadataBuilder = android::fs_mgr::MetadataBuilder;
    using DeltaArchiveManifest = chromeos_update_engine::DeltaArchiveManifest;

  public:
    // Dependency injection for testing.
    class IDeviceInfo {
      public:
        virtual ~IDeviceInfo() {}
        virtual std::string GetGsidDir() const = 0;
        virtual std::string GetMetadataDir() const = 0;
        virtual std::string GetSlotSuffix() const = 0;
        virtual std::string GetOtherSlotSuffix() const = 0;
        virtual std::string GetSuperDevice(uint32_t slot) const = 0;
        virtual const IPartitionOpener& GetPartitionOpener() const = 0;
        virtual bool IsOverlayfsSetup() const = 0;
    };

    ~SnapshotManager();

    // Return a new SnapshotManager instance, or null on error. The device
    // pointer is owned for the lifetime of SnapshotManager. If null, a default
    // instance will be created.
    static std::unique_ptr<SnapshotManager> New(IDeviceInfo* device = nullptr);

    // This is similar to New(), except designed specifically for first-stage
    // init.
    static std::unique_ptr<SnapshotManager> NewForFirstStageMount(IDeviceInfo* device = nullptr);

    // Helper function for first-stage init to check whether a SnapshotManager
    // might be needed to perform first-stage mounts.
    static bool IsSnapshotManagerNeeded();

    // Begin an update. This must be called before creating any snapshots. It
    // will fail if GetUpdateState() != None.
    bool BeginUpdate();

    // Cancel an update; any snapshots will be deleted. This is allowed if the
    // state == Initiated, None, or Unverified (before rebooting to the new
    // slot).
    bool CancelUpdate();

    // Mark snapshot writes as having completed. After this, new snapshots cannot
    // be created, and the device must either cancel the OTA (either before
    // rebooting or after rolling back), or merge the OTA.
    bool FinishedSnapshotWrites();

    // Initiate a merge on all snapshot devices. This should only be used after an
    // update has been marked successful after booting.
    bool InitiateMerge();

    // Perform any necessary post-boot actions. This should be run soon after
    // /data is mounted.
    //
    // If a merge is in progress, this function will block until the merge is
    // completed. If a merge or update was cancelled, this will clean up any
    // update artifacts and return.
    //
    // Note that after calling this, GetUpdateState() may still return that a
    // merge is in progress:
    //   MergeFailed indicates that a fatal error occurred. WaitForMerge() may
    //   called any number of times again to attempt to make more progress, but
    //   we do not expect it to succeed if a catastrophic error occurred.
    //
    //   MergeNeedsReboot indicates that the merge has completed, but cleanup
    //   failed. This can happen if for some reason resources were not closed
    //   properly. In this case another reboot is needed before we can take
    //   another OTA. However, WaitForMerge() can be called again without
    //   rebooting, to attempt to finish cleanup anyway.
    //
    //   MergeCompleted indicates that the update has fully completed.
    //   GetUpdateState will return None, and a new update can begin.
    UpdateState ProcessUpdateState();

    // Find the status of the current update, if any.
    //
    // |progress| depends on the returned status:
    //   Merging: Value in the range [0, 100]
    //   MergeCompleted: 100
    //   Other: 0
    UpdateState GetUpdateState(double* progress = nullptr);

    // Create necessary COW device / files for OTA clients. New logical partitions will be added to
    // group "cow" in target_metadata. Regions of partitions of current_metadata will be
    // "write-protected" and snapshotted.
    bool CreateUpdateSnapshots(const DeltaArchiveManifest& manifest);

    // Map a snapshotted partition for OTA clients to write to. Write-protected regions are
    // determined previously in CreateSnapshots.
    bool MapUpdateSnapshot(const CreateLogicalPartitionParams& params, std::string* snapshot_path);

    // Unmap a snapshot device that's previously mapped with MapUpdateSnapshot.
    bool UnmapUpdateSnapshot(const std::string& target_partition_name);

    // If this returns true, first-stage mount must call
    // CreateLogicalAndSnapshotPartitions rather than CreateLogicalPartitions.
    bool NeedSnapshotsInFirstStageMount();

    // Perform first-stage mapping of snapshot targets. This replaces init's
    // call to CreateLogicalPartitions when snapshots are present.
    bool CreateLogicalAndSnapshotPartitions(const std::string& super_device);

    // Dump debug information.
    bool Dump(std::ostream& os);

  private:
    FRIEND_TEST(SnapshotTest, CleanFirstStageMount);
    FRIEND_TEST(SnapshotTest, CreateSnapshot);
    FRIEND_TEST(SnapshotTest, FirstStageMountAfterRollback);
    FRIEND_TEST(SnapshotTest, FirstStageMountAndMerge);
    FRIEND_TEST(SnapshotTest, FlashSuperDuringMerge);
    FRIEND_TEST(SnapshotTest, FlashSuperDuringUpdate);
    FRIEND_TEST(SnapshotTest, MapPartialSnapshot);
    FRIEND_TEST(SnapshotTest, MapSnapshot);
    FRIEND_TEST(SnapshotTest, Merge);
    FRIEND_TEST(SnapshotTest, MergeCannotRemoveCow);
    FRIEND_TEST(SnapshotTest, NoMergeBeforeReboot);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    friend class SnapshotTest;
    friend class SnapshotUpdateTest;
    friend struct AutoDeleteCowImage;
    friend struct AutoDeleteSnapshot;
    friend struct PartitionCowCreator;

    using DmTargetSnapshot = android::dm::DmTargetSnapshot;
    using IImageManager = android::fiemap::IImageManager;
    using TargetInfo = android::dm::DeviceMapper::TargetInfo;

    explicit SnapshotManager(IDeviceInfo* info);

    // This is created lazily since it can connect via binder.
    bool EnsureImageManager();

    // Helper for first-stage init.
    bool ForceLocalImageManager();

    // Helper function for tests.
    IImageManager* image_manager() const { return images_.get(); }

    // Since libsnapshot is included into multiple processes, we flock() our
    // files for simple synchronization. LockedFile is a helper to assist with
    // this. It also serves as a proof-of-lock for some functions.
    class LockedFile final {
      public:
        LockedFile(const std::string& path, android::base::unique_fd&& fd, int lock_mode)
            : path_(path), fd_(std::move(fd)), lock_mode_(lock_mode) {}
        ~LockedFile();

        const std::string& path() const { return path_; }
        int fd() const { return fd_; }
        int lock_mode() const { return lock_mode_; }

      private:
        std::string path_;
        android::base::unique_fd fd_;
        int lock_mode_;
    };
    std::unique_ptr<LockedFile> OpenFile(const std::string& file, int open_flags, int lock_flags);
    bool Truncate(LockedFile* file);

    enum class SnapshotState : int { None, Created, Merging, MergeCompleted };
    static std::string to_string(SnapshotState state);

    // This state is persisted per-snapshot in /metadata/ota/snapshots/.
    struct SnapshotStatus {
        SnapshotState state = SnapshotState::None;
        uint64_t device_size = 0;
        uint64_t snapshot_size = 0;
        uint64_t cow_partition_size = 0;
        uint64_t cow_file_size = 0;

        // These are non-zero when merging.
        uint64_t sectors_allocated = 0;
        uint64_t metadata_sectors = 0;
    };

    // Create a new snapshot record. This creates the backing COW store and
    // persists information needed to map the device. The device can be mapped
    // with MapSnapshot().
    //
    // |status|.device_size should be the size of the base_device that will be passed
    // via MapDevice(). |status|.snapshot_size should be the number of bytes in the
    // base device, starting from 0, that will be snapshotted. |status|.cow_file_size
    // should be the amount of space that will be allocated to store snapshot
    // deltas.
    //
    // If |status|.snapshot_size < |status|.device_size, then the device will always
    // be mapped with two table entries: a dm-snapshot range covering
    // snapshot_size, and a dm-linear range covering the remainder.
    //
    // All sizes are specified in bytes, and the device, snapshot, COW partition and COW file sizes
    // must be a multiple of the sector size (512 bytes).
    bool CreateSnapshot(LockedFile* lock, const std::string& name, SnapshotStatus status);

    // |name| should be the base partition name (e.g. "system_a"). Create the
    // backing COW image using the size previously passed to CreateSnapshot().
    bool CreateCowImage(LockedFile* lock, const std::string& name);

    // Map a snapshot device that was previously created with CreateSnapshot.
    // If a merge was previously initiated, the device-mapper table will have a
    // snapshot-merge target instead of a snapshot target. If the timeout
    // parameter greater than zero, this function will wait the given amount
    // of time for |dev_path| to become available, and fail otherwise. If
    // timeout_ms is 0, then no wait will occur and |dev_path| may not yet
    // exist on return.
    bool MapSnapshot(LockedFile* lock, const std::string& name, const std::string& base_device,
                     const std::string& cow_device, const std::chrono::milliseconds& timeout_ms,
                     std::string* dev_path);

    // Map a COW image that was previous created with CreateCowImage.
    bool MapCowImage(const std::string& name, const std::chrono::milliseconds& timeout_ms);

    // Remove the backing copy-on-write image and snapshot states for the named snapshot. The
    // caller is responsible for ensuring that the snapshot is unmapped.
    bool DeleteSnapshot(LockedFile* lock, const std::string& name);

    // Unmap a snapshot device previously mapped with MapSnapshotDevice().
    bool UnmapSnapshot(LockedFile* lock, const std::string& name);

    // Unmap a COW image device previously mapped with MapCowImage().
    bool UnmapCowImage(const std::string& name);

    // Unmap and remove all known snapshots.
    bool RemoveAllSnapshots(LockedFile* lock);

    // List the known snapshot names.
    bool ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots);

    // Check for a cancelled or rolled back merge, returning true if such a
    // condition was detected and handled.
    bool HandleCancelledUpdate(LockedFile* lock);

    // Remove artifacts created by the update process, such as snapshots, and
    // set the update state to None.
    bool RemoveAllUpdateState(LockedFile* lock);

    // Interact with /metadata/ota/state.
    std::unique_ptr<LockedFile> OpenStateFile(int open_flags, int lock_flags);
    std::unique_ptr<LockedFile> LockShared();
    std::unique_ptr<LockedFile> LockExclusive();
    UpdateState ReadUpdateState(LockedFile* file);
    bool WriteUpdateState(LockedFile* file, UpdateState state);
    std::string GetStateFilePath() const;

    // Helpers for merging.
    bool SwitchSnapshotToMerge(LockedFile* lock, const std::string& name);
    bool RewriteSnapshotDeviceTable(const std::string& dm_name);
    bool MarkSnapshotMergeCompleted(LockedFile* snapshot_lock, const std::string& snapshot_name);
    void AcknowledgeMergeSuccess(LockedFile* lock);
    void AcknowledgeMergeFailure();
    bool IsCancelledSnapshot(const std::string& snapshot_name);

    // Note that these require the name of the device containing the snapshot,
    // which may be the "inner" device. Use GetsnapshotDeviecName().
    bool QuerySnapshotStatus(const std::string& dm_name, std::string* target_type,
                             DmTargetSnapshot::Status* status);
    bool IsSnapshotDevice(const std::string& dm_name, TargetInfo* target = nullptr);

    // Internal callback for when merging is complete.
    bool OnSnapshotMergeComplete(LockedFile* lock, const std::string& name,
                                 const SnapshotStatus& status);
    bool CollapseSnapshotDevice(const std::string& name, const SnapshotStatus& status);

    // Only the following UpdateStates are used here:
    //   UpdateState::Merging
    //   UpdateState::MergeCompleted
    //   UpdateState::MergeFailed
    //   UpdateState::MergeNeedsReboot
    UpdateState CheckMergeState();
    UpdateState CheckMergeState(LockedFile* lock);
    UpdateState CheckTargetMergeState(LockedFile* lock, const std::string& name);

    // Interact with status files under /metadata/ota/snapshots.
    bool WriteSnapshotStatus(LockedFile* lock, const std::string& name,
                             const SnapshotStatus& status);
    bool ReadSnapshotStatus(LockedFile* lock, const std::string& name, SnapshotStatus* status);
    std::string GetSnapshotStatusFilePath(const std::string& name);

    std::string GetSnapshotBootIndicatorPath();
    void RemoveSnapshotBootIndicator();

    // Return the name of the device holding the "snapshot" or "snapshot-merge"
    // target. This may not be the final device presented via MapSnapshot(), if
    // for example there is a linear segment.
    std::string GetSnapshotDeviceName(const std::string& snapshot_name,
                                      const SnapshotStatus& status);

    // Map the base device, COW devices, and snapshot device.
    bool MapPartitionWithSnapshot(LockedFile* lock, CreateLogicalPartitionParams params,
                                  std::string* path);

    // Map the COW devices, including the partition in super and the images.
    // |params|:
    //    - |partition_name| should be the name of the top-level partition (e.g. system_b),
    //            not system_b-cow-img
    //    - |device_name| and |partition| is ignored
    //    - |timeout_ms| and the rest is respected
    // Return the path in |cow_device_path| (e.g. /dev/block/dm-1) and major:minor in
    // |cow_device_string|
    bool MapCowDevices(LockedFile* lock, const CreateLogicalPartitionParams& params,
                       const SnapshotStatus& snapshot_status, AutoDeviceList* created_devices,
                       std::string* cow_name);

    // The reverse of MapCowDevices.
    bool UnmapCowDevices(LockedFile* lock, const std::string& name);

    // The reverse of MapPartitionWithSnapshot.
    bool UnmapPartitionWithSnapshot(LockedFile* lock, const std::string& target_partition_name);

    // If there isn't a previous update, return true. |needs_merge| is set to false.
    // If there is a previous update but the device has not boot into it, tries to cancel the
    //   update and delete any snapshots. Return true if successful. |needs_merge| is set to false.
    // If there is a previous update and the device has boot into it, do nothing and return true.
    //   |needs_merge| is set to true.
    bool TryCancelUpdate(bool* needs_merge);

    std::string gsid_dir_;
    std::string metadata_dir_;
    std::unique_ptr<IDeviceInfo> device_;
    std::unique_ptr<IImageManager> images_;
    bool has_local_image_manager_ = false;
};

}  // namespace snapshot
}  // namespace android

#ifdef DEFINED_FRIEND_TEST
#undef DEFINED_FRIEND_TEST
#undef FRIEND_TEST
#endif
