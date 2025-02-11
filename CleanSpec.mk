# Copyright (C) 2007 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# If you don't need to do a full clean build but would like to touch
# a file or delete some intermediate files, add a clean step to the end
# of the list.  These steps will only be run once, if they haven't been
# run before.
#
# E.g.:
#     $(call add-clean-step, touch -c external/sqlite/sqlite3.h)
#     $(call add-clean-step, rm -rf $(PRODUCT_OUT)/obj/STATIC_LIBRARIES/libz_intermediates)
#
# Always use "touch -c" and "rm -f" or "rm -rf" to gracefully deal with
# files that are missing or have been moved.
#
# Use $(PRODUCT_OUT) to get to the "out/target/product/blah/" directory.
# Use $(OUT_DIR) to refer to the "out" directory.
#
# If you need to re-do something that's already mentioned, just copy
# the command and add it to the bottom of the list.  E.g., if a change
# that you made last week required touching a file and a change you
# made today requires touching the same file, just copy the old
# touch step and add it to the end of the list.
#
# ************************************************
# NEWER CLEAN STEPS MUST BE AT THE END OF THE LIST
# ************************************************

# For example:
#$(call add-clean-step, rm -rf $(OUT_DIR)/target/common/obj/APPS/AndroidTests_intermediates)
#$(call add-clean-step, rm -rf $(OUT_DIR)/target/common/obj/JAVA_LIBRARIES/core_intermediates)
#$(call add-clean-step, find $(OUT_DIR) -type f -name "IGTalkSession*" -print0 | xargs -0 rm -f)
#$(call add-clean-step, rm -rf $(PRODUCT_OUT)/data/*)

# ************************************************
# NEWER CLEAN STEPS MUST BE AT THE END OF THE LIST
# ************************************************

$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/init.rc)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/init.rc)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/bin/reboot)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/default.prop)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/recovery/root/default.prop)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/obj/EXECUTABLES/lmkd_intermediates/import_includes)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/obj/SHARED_LIBRARIES/libsysutils_intermediates/import_includes)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/bin/grep $(PRODUCT_OUT)/system/bin/toolbox)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib/hw/gatekeeper.$(TARGET_DEVICE).so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib64/hw/gatekeeper.$(TARGET_DEVICE).so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/vendor)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/init.rc)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib/libtrusty.so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib64/libtrusty.so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib/hw/keystore.trusty.so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib64/hw/keystore.trusty.so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib/hw/gatekeeper.trusty.so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/lib64/hw/gatekeeper.trusty.so)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/bin/secure-storage-unit-test)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/bin/storageproxyd)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/bin/tipc-test)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/bin/trusty_keymaster_tipc)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/root)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/etc/ld.config.txt)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/etc/llndk.libraries.txt)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/etc/vndksp.libraries.txt)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/etc/ld.config.txt)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/etc/llndk.libraries.txt)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/etc/vndksp.libraries.txt)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/recovery/root/)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/sbin/charger)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/recovery/root/sbin/charger)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/sbin)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/recovery/root/sbin)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/product_services)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/product_services.img)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/system/product_services)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/product_services)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/recovery/root/product_services)
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/debug_ramdisk/product_services)
