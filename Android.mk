ifneq ($(filter nozomi,$(TARGET_DEVICE)),)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

KERNEL_DEFCONFIG := fuji_nozomi_defconfig
KERNEL_DIR := $(ANDROID_BUILD_TOP)/kernel/nozomi
-include $(KERNEL_DIR)/AndroidKernel.mk

LOCAL_PREBUILT_MODULE_FILE := $(TARGET_PREBUILT_KERNEL)

LOCAL_MODULE       := kernel
LOCAL_MODULE_TAGS  := optional
LOCAL_MODULE_CLASS := KERNEL_OBJ
LOCAL_MODULE_PATH  := $(PRODUCT_OUT)
include $(BUILD_PREBUILT)

endif

