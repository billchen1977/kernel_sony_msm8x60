ifneq ($(filter nozomi,$(TARGET_DEVICE)),)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

KERNEL_DEFCONFIG := fuji_nozomi_defconfig
KERNEL_DIR := $(ANDROID_BUILD_TOP)/kernel/nozomi
-include $(KERNEL_DIR)/AndroidKernel.mk

$(TARGET_DEVICE_DIR)/kernel: $(TARGET_PREBUILT_KERNEL)
	cp $(TARGET_PREBUILT_KERNEL) $(TARGET_DEVICE_DIR)/kernel
	mkdir -p $(PRODUCT_OUT)/obj/KERNEL_OBJ/kernel_intermediates
	cp $(TARGET_PREBUILT_KERNEL) $(PRODUCT_OUT)/obj/KERNEL_OBJ/kernel_intermediates/kernel

kernel: $(TARGET_DEVICE_DIR)/kernel

LOCAL_MODULE       := kernel
LOCAL_MODULE_TAGS  := optional
LOCAL_MODULE_CLASS := KERNEL_OBJ
LOCAL_MODULE_PATH  := $(PRODUCT_OUT)
LOCAL_SRC_FILES    := kernel
include $(BUILD_PREBUILT)

endif

