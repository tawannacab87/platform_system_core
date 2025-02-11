# Copyright 2013 The Android Open Source Project

LOCAL_PATH := $(call my-dir)

### charger ###
include $(CLEAR_VARS)
ifeq ($(strip $(BOARD_CHARGER_NO_UI)),true)
LOCAL_CHARGER_NO_UI := true
endif

LOCAL_SRC_FILES := \
    charger.cpp \

LOCAL_MODULE := charger
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include

LOCAL_CFLAGS := -Werror

CHARGER_STATIC_LIBRARIES := \
    android.hardware.health@2.0-impl \
    android.hardware.health@1.0-convert \
    libbinderthreadstate \
    libcharger_sysprop \
    libhidlbase \
    libhealthstoragedefault \
    libminui \
    libvndksupport \
    libhealthd_charger \
    libhealthd_charger_nops \
    libhealthd_draw \
    libbatterymonitor \

CHARGER_SHARED_LIBRARIES := \
    android.hardware.health@2.0 \
    libbase \
    libcutils \
    libjsoncpp \
    libpng \
    libprocessgroup \
    liblog \
    libutils \

CHARGER_SHARED_LIBRARIES += libsuspend

LOCAL_STATIC_LIBRARIES := $(CHARGER_STATIC_LIBRARIES)
LOCAL_SHARED_LIBRARIES := $(CHARGER_SHARED_LIBRARIES)

LOCAL_HAL_STATIC_LIBRARIES := libhealthd

# Symlink /charger to /system/bin/charger
LOCAL_POST_INSTALL_CMD := $(hide) mkdir -p $(TARGET_ROOT_OUT) \
    && ln -sf /system/bin/charger $(TARGET_ROOT_OUT)/charger

include $(BUILD_EXECUTABLE)

### charger.recovery ###
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    charger.cpp \

LOCAL_MODULE := charger.recovery
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/system/bin
LOCAL_MODULE_STEM := charger

LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_CFLAGS := -Wall -Werror -DCHARGER_FORCE_NO_UI=1

# charger.recovery doesn't link against libhealthd_{charger,draw} or libminui, since it doesn't need
# any UI support.
LOCAL_STATIC_LIBRARIES := \
    android.hardware.health@2.0-impl \
    android.hardware.health@1.0-convert \
    libbinderthreadstate \
    libcharger_sysprop \
    libhidlbase \
    libhealthstoragedefault \
    libvndksupport \
    libhealthd_charger_nops \
    libbatterymonitor \

# These shared libs will be installed to recovery image because of the dependency in `recovery`
# module.
LOCAL_SHARED_LIBRARIES := \
    android.hardware.health@2.0 \
    libbase \
    libcutils \
    liblog \
    libutils \

# The use of LOCAL_HAL_STATIC_LIBRARIES prevents from building this module with Android.bp.
LOCAL_HAL_STATIC_LIBRARIES := libhealthd

include $(BUILD_EXECUTABLE)

### charger_test ###
include $(CLEAR_VARS)
LOCAL_MODULE := charger_test
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_CFLAGS := -Wall -Werror
LOCAL_STATIC_LIBRARIES := $(CHARGER_STATIC_LIBRARIES)
LOCAL_SHARED_LIBRARIES := $(CHARGER_SHARED_LIBRARIES)
LOCAL_SRC_FILES := \
    charger_test.cpp \

include $(BUILD_EXECUTABLE)

CHARGER_STATIC_LIBRARIES :=
CHARGER_SHARED_LIBRARIES :=

### charger_res_images ###
ifneq ($(strip $(LOCAL_CHARGER_NO_UI)),true)
define _add-charger-image
include $$(CLEAR_VARS)
LOCAL_MODULE := system_core_charger_res_images_$(notdir $(1))
LOCAL_MODULE_STEM := $(notdir $(1))
_img_modules += $$(LOCAL_MODULE)
LOCAL_SRC_FILES := $1
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $$(TARGET_ROOT_OUT)/res/images/charger
include $$(BUILD_PREBUILT)
endef

_img_modules :=
_images :=
$(foreach _img, $(call find-subdir-subdir-files, "images", "*.png"), \
  $(eval $(call _add-charger-image,$(_img))))

include $(CLEAR_VARS)
LOCAL_MODULE := charger_res_images
LOCAL_MODULE_TAGS := optional
LOCAL_REQUIRED_MODULES := $(_img_modules)
include $(BUILD_PHONY_PACKAGE)

_add-charger-image :=
_img_modules :=
endif # LOCAL_CHARGER_NO_UI
