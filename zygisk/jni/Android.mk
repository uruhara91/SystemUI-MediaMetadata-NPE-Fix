LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := systemui_media_fix
LOCAL_SRC_FILES := module.cpp binder.cpp
LOCAL_CPPFLAGS := \
    -std=c++20 \
    -O2 \
    -flto=thin \
    -DNDEBUG \
    -D_FORTIFY_SOURCE=2 \
    -fno-exceptions \
    -fno-rtti \
    -fno-threadsafe-statics \
    -fno-semantic-interposition \
    -fstack-protector-strong \
    -fvisibility=hidden \
    -fvisibility-inlines-hidden \
    -ffunction-sections \
    -fdata-sections \
    -Wall \
    -Wextra \
    -Wpedantic
LOCAL_LDFLAGS := \
    -flto=thin \
    -Wl,--gc-sections \
    -Wl,--icf=safe \
    -Wl,--as-needed \
    -Wl,--exclude-libs,ALL \
    -Wl,-z,relro \
    -Wl,-z,now \
    -Wl,-z,noexecstack
LOCAL_LDLIBS := -llog -landroid -ldl
include $(BUILD_SHARED_LIBRARY)
