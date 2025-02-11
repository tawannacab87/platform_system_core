#####################################################################
# Builds linker config file, ld.config.txt, from the specified template
# under $(LOCAL_PATH)/etc/*.
#
# Inputs:
#   (expected to follow an include of $(BUILD_SYSTEM)/base_rules.mk)
#   ld_config_template: template linker config file to use,
#                       e.g. $(LOCAL_PATH)/etc/ld.config.txt
#   vndk_version: version of the VNDK library lists used to update the
#                 template linker config file, e.g. 28
#   lib_list_from_prebuilts: should be set to 'true' if the VNDK library
#                            lists should be read from /prebuilts/vndk/*
#   libz_is_llndk: should be set to 'true' if libz must be included in
#                  llndk and not in vndk-sp
# Outputs:
#   Builds and installs ld.config.$VER.txt or ld.config.vndk_lite.txt
#####################################################################

# Read inputs
ld_config_template := $(strip $(ld_config_template))
check_backward_compatibility := $(strip $(check_backward_compatibility))
vndk_version := $(strip $(vndk_version))
lib_list_from_prebuilts := $(strip $(lib_list_from_prebuilts))
libz_is_llndk := $(strip $(libz_is_llndk))

my_vndk_use_core_variant := $(TARGET_VNDK_USE_CORE_VARIANT)
ifeq ($(lib_list_from_prebuilts),true)
my_vndk_use_core_variant := false
endif

compatibility_check_script := \
  $(LOCAL_PATH)/ld_config_backward_compatibility_check.py
intermediates_dir := $(call intermediates-dir-for,ETC,$(LOCAL_MODULE))
library_lists_dir := $(intermediates_dir)
ifeq ($(lib_list_from_prebuilts),true)
  library_lists_dir := prebuilts/vndk/v$(vndk_version)/$(TARGET_ARCH)/configs
endif

llndk_libraries_file := $(library_lists_dir)/llndk.libraries.$(vndk_version).txt
vndksp_libraries_file := $(library_lists_dir)/vndksp.libraries.$(vndk_version).txt
vndkcore_libraries_file := $(library_lists_dir)/vndkcore.libraries.txt
vndkprivate_libraries_file := $(library_lists_dir)/vndkprivate.libraries.txt
llndk_moved_to_apex_libraries_file := $(library_lists_dir)/llndkinapex.libraries.txt
ifeq ($(my_vndk_use_core_variant),true)
vndk_using_core_variant_libraries_file := $(library_lists_dir)/vndk_using_core_variant.libraries.$(vndk_version).txt
endif

sanitizer_runtime_libraries := $(call normalize-path-list,$(addsuffix .so,\
  $(ADDRESS_SANITIZER_RUNTIME_LIBRARY) \
  $(HWADDRESS_SANITIZER_RUNTIME_LIBRARY) \
  $(UBSAN_RUNTIME_LIBRARY) \
  $(TSAN_RUNTIME_LIBRARY) \
  $(2ND_ADDRESS_SANITIZER_RUNTIME_LIBRARY) \
  $(2ND_HWADDRESS_SANITIZER_RUNTIME_LIBRARY) \
  $(2ND_UBSAN_RUNTIME_LIBRARY) \
  $(2ND_TSAN_RUNTIME_LIBRARY)))
# If BOARD_VNDK_VERSION is not defined, VNDK version suffix will not be used.
vndk_version_suffix := $(if $(vndk_version),-$(vndk_version))

ifneq ($(lib_list_from_prebuilts),true)
ifeq ($(libz_is_llndk),true)
  llndk_libraries_list := $(LLNDK_LIBRARIES) libz
  vndksp_libraries_list := $(filter-out libz,$(VNDK_SAMEPROCESS_LIBRARIES))
else
  llndk_libraries_list := $(LLNDK_LIBRARIES)
  vndksp_libraries_list := $(VNDK_SAMEPROCESS_LIBRARIES)
endif

# LLNDK libraries that has been moved to an apex package and no longer are present on
# /system image.
llndk_libraries_moved_to_apex_list:=$(LLNDK_MOVED_TO_APEX_LIBRARIES)

# Returns the unique installed basenames of a module, or module.so if there are
# none.  The guess is to handle cases like libc, where the module itself is
# marked uninstallable but a symlink is installed with the name libc.so.
# $(1): list of libraries
# $(2): suffix to to add to each library (not used for guess)
define module-installed-files-or-guess
$(foreach lib,$(1),$(or $(strip $(sort $(notdir $(call module-installed-files,$(lib)$(2))))),$(lib).so))
endef

# $(1): list of libraries
# $(2): suffix to add to each library
# $(3): output file to write the list of libraries to
define write-libs-to-file
$(3): PRIVATE_LIBRARIES := $(1)
$(3): PRIVATE_SUFFIX := $(2)
$(3):
	echo -n > $$@ && $$(foreach so,$$(call module-installed-files-or-guess,$$(PRIVATE_LIBRARIES),$$(PRIVATE_SUFFIX)),echo $$(so) >> $$@;)
endef
$(eval $(call write-libs-to-file,$(llndk_libraries_list),,$(llndk_libraries_file)))
$(eval $(call write-libs-to-file,$(vndksp_libraries_list),.vendor,$(vndksp_libraries_file)))
$(eval $(call write-libs-to-file,$(VNDK_CORE_LIBRARIES),.vendor,$(vndkcore_libraries_file)))
$(eval $(call write-libs-to-file,$(VNDK_PRIVATE_LIBRARIES),.vendor,$(vndkprivate_libraries_file)))
ifeq ($(my_vndk_use_core_variant),true)
$(eval $(call write-libs-to-file,$(VNDK_USING_CORE_VARIANT_LIBRARIES),,$(vndk_using_core_variant_libraries_file)))
endif
endif # ifneq ($(lib_list_from_prebuilts),true)

# Given a file with a list of libs, filter-out the VNDK private libraries
# and write resulting list to a new file in "a:b:c" format
#
# $(1): libs file from which to filter-out VNDK private libraries
# $(2): output file with the filtered list of lib names
$(LOCAL_BUILT_MODULE): private-filter-out-private-libs = \
  paste -sd ":" $(1) > $(2) && \
  while read -r privatelib; do sed -i.bak "s/$$privatelib//" $(2) ; done < $(PRIVATE_VNDK_PRIVATE_LIBRARIES_FILE) && \
  sed -i.bak -e 's/::\+/:/g ; s/^:\+// ; s/:\+$$//' $(2) && \
  rm -f $(2).bak

# # Given a file with a list of libs in "a:b:c" format, filter-out the LLNDK libraries migrated into apex file
# # and write resulting list to a new file in "a:b:c" format
 $(LOCAL_BUILT_MODULE): private-filter-out-llndk-in-apex-libs = \
   for lib in $(PRIVATE_LLNDK_LIBRARIES_MOVED_TO_APEX_LIST); do sed -i.bak s/$$lib.so// $(1); done && \
   sed -i.bak -e 's/::\+/:/g ; s/^:\+// ; s/:\+$$//' $(1) && \
   rm -f $(1).bak

$(LOCAL_BUILT_MODULE): PRIVATE_LLNDK_LIBRARIES_FILE := $(llndk_libraries_file)
$(LOCAL_BUILT_MODULE): PRIVATE_VNDK_SP_LIBRARIES_FILE := $(vndksp_libraries_file)
$(LOCAL_BUILT_MODULE): PRIVATE_VNDK_CORE_LIBRARIES_FILE := $(vndkcore_libraries_file)
$(LOCAL_BUILT_MODULE): PRIVATE_VNDK_PRIVATE_LIBRARIES_FILE := $(vndkprivate_libraries_file)
$(LOCAL_BUILT_MODULE): PRIVATE_SANITIZER_RUNTIME_LIBRARIES := $(sanitizer_runtime_libraries)
$(LOCAL_BUILT_MODULE): PRIVATE_VNDK_VERSION_SUFFIX := $(vndk_version_suffix)
$(LOCAL_BUILT_MODULE): PRIVATE_INTERMEDIATES_DIR := $(intermediates_dir)
$(LOCAL_BUILT_MODULE): PRIVATE_COMP_CHECK_SCRIPT := $(compatibility_check_script)
$(LOCAL_BUILT_MODULE): PRIVATE_VNDK_VERSION_TAG := \#VNDK$(vndk_version)\#
$(LOCAL_BUILT_MODULE): PRIVATE_LLNDK_LIBRARIES_MOVED_TO_APEX_LIST := $(llndk_libraries_moved_to_apex_list)
deps := $(llndk_libraries_file) $(vndksp_libraries_file) $(vndkcore_libraries_file) \
  $(vndkprivate_libraries_file)
ifeq ($(check_backward_compatibility),true)
deps += $(compatibility_check_script) $(wildcard prebuilts/vndk/*/*/configs/ld.config.*.txt)
endif
ifeq ($(my_vndk_use_core_variant),true)
$(LOCAL_BUILT_MODULE): PRIVATE_VNDK_USING_CORE_VARIANT_LIBRARIES_FILE := $(vndk_using_core_variant_libraries_file)
deps += $(vndk_using_core_variant_libraries_file)
endif

$(LOCAL_BUILT_MODULE): $(ld_config_template) $(deps)
	@echo "Generate: $< -> $@"
ifeq ($(check_backward_compatibility),true)
	@echo "Checking backward compatibility..."
	$(hide) $(PRIVATE_COMP_CHECK_SCRIPT) $<
endif
	@mkdir -p $(dir $@)
	$(call private-filter-out-private-libs,$(PRIVATE_LLNDK_LIBRARIES_FILE),$(PRIVATE_INTERMEDIATES_DIR)/llndk_filtered)
	$(call private-filter-out-llndk-in-apex-libs,$(PRIVATE_INTERMEDIATES_DIR)/llndk_filtered)
	$(hide) sed -e "s?%LLNDK_LIBRARIES%?$$(cat $(PRIVATE_INTERMEDIATES_DIR)/llndk_filtered)?g" $< >$@
	$(call private-filter-out-private-libs,$(PRIVATE_VNDK_SP_LIBRARIES_FILE),$(PRIVATE_INTERMEDIATES_DIR)/vndksp_filtered)
	$(hide) sed -i.bak -e "s?%VNDK_SAMEPROCESS_LIBRARIES%?$$(cat $(PRIVATE_INTERMEDIATES_DIR)/vndksp_filtered)?g" $@
	$(call private-filter-out-private-libs,$(PRIVATE_VNDK_CORE_LIBRARIES_FILE),$(PRIVATE_INTERMEDIATES_DIR)/vndkcore_filtered)
	$(hide) sed -i.bak -e "s?%VNDK_CORE_LIBRARIES%?$$(cat $(PRIVATE_INTERMEDIATES_DIR)/vndkcore_filtered)?g" $@

ifeq ($(my_vndk_use_core_variant),true)
	$(call private-filter-out-private-libs,$(PRIVATE_VNDK_USING_CORE_VARIANT_LIBRARIES_FILE),$(PRIVATE_INTERMEDIATES_DIR)/vndk_using_core_variant_filtered)
	$(hide) sed -i.bak -e "s?%VNDK_IN_SYSTEM_NS%?,vndk_in_system?g" $@
	$(hide) sed -i.bak -e "s?%VNDK_USING_CORE_VARIANT_LIBRARIES%?$$(cat $(PRIVATE_INTERMEDIATES_DIR)/vndk_using_core_variant_filtered)?g" $@
else
	$(hide) sed -i.bak -e "s?%VNDK_IN_SYSTEM_NS%??g" $@
	# Unlike LLNDK or VNDK-SP, VNDK_USING_CORE_VARIANT_LIBRARIES can be nothing
	# if TARGET_VNDK_USE_CORE_VARIANT is not set.  In this case, we need to remove
	# the entire line in the linker config so that we are not left with a line
	# like:
	#   namespace.vndk.link.vndk_in_system.shared_libs =
	$(hide) sed -i.bak -e 's?^.*= %VNDK_USING_CORE_VARIANT_LIBRARIES%$$??' $@
endif

	$(hide) echo -n > $(PRIVATE_INTERMEDIATES_DIR)/private_llndk && \
	while read -r privatelib; \
	do (grep $$privatelib $(PRIVATE_LLNDK_LIBRARIES_FILE) || true) >> $(PRIVATE_INTERMEDIATES_DIR)/private_llndk ; \
	done < $(PRIVATE_VNDK_PRIVATE_LIBRARIES_FILE) && \
	paste -sd ":" $(PRIVATE_INTERMEDIATES_DIR)/private_llndk | \
	sed -i.bak -e "s?%PRIVATE_LLNDK_LIBRARIES%?$$(cat -)?g" $@

	$(hide) sed -i.bak -e "s?%SANITIZER_RUNTIME_LIBRARIES%?$(PRIVATE_SANITIZER_RUNTIME_LIBRARIES)?g" $@
	$(hide) sed -i.bak -e "s?%VNDK_VER%?$(PRIVATE_VNDK_VERSION_SUFFIX)?g" $@
	$(hide) sed -i.bak -e "s?%PRODUCT%?$(TARGET_COPY_OUT_PRODUCT)?g" $@
	$(hide) sed -i.bak -e "s?%SYSTEM_EXT%?$(TARGET_COPY_OUT_SYSTEM_EXT)?g" $@
	$(hide) sed -i.bak -e "s?^$(PRIVATE_VNDK_VERSION_TAG)??g" $@
	$(hide) sed -i.bak "/^\#VNDK[0-9]\{2\}\#.*$$/d" $@
	$(hide) rm -f $@.bak

ld_config_template :=
check_backward_compatibility :=
vndk_version :=
lib_list_from_prebuilts :=
libz_is_llndk :=
compatibility_check_script :=
intermediates_dir :=
library_lists_dir :=
llndk_libraries_file :=
llndk_moved_to_apex_libraries_file :=
vndksp_libraries_file :=
vndkcore_libraries_file :=
vndkprivate_libraries_file :=
deps :=
sanitizer_runtime_libraries :=
vndk_version_suffix :=
llndk_libraries_list :=
vndksp_libraries_list :=
write-libs-to-file :=

ifeq ($(my_vndk_use_core_variant),true)
vndk_using_core_variant_libraries_file :=
vndk_using_core_variant_libraries_list :=
endif

my_vndk_use_core_variant :=
