#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#include "binder.hpp"
#include "zygisk.hpp"

#define LOGI(fmt, ...) \
    __android_log_print(ANDROID_LOG_INFO, "SystemUIMediaFix", "[%d] " fmt, \
                        __LINE__ __VA_OPT__(,) __VA_ARGS__)
#define LOGE(fmt, ...) \
    __android_log_print(ANDROID_LOG_ERROR, "SystemUIMediaFix", "[%d] " fmt, \
                        __LINE__ __VA_OPT__(,) __VA_ARGS__)

#define LIKELY(value) __builtin_expect(!!(value), 1)
#define UNLIKELY(value) __builtin_expect(!!(value), 0)
#define HOT __attribute__((hot))
#define COLD __attribute__((cold, noinline))
#define ALWAYS_INLINE inline __attribute__((always_inline))

namespace {

constexpr int kSupportedSdk = 31;
constexpr char kTargetProcess[] = "com.android.systemui";
constexpr char kSupportedFingerprint[] =
    "Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys";
constexpr char16_t kSessionControllerDescriptor[] =
    u"android.media.session.ISessionController";
constexpr size_t kSessionControllerDescriptorLength =
    (sizeof(kSessionControllerDescriptor) / sizeof(kSessionControllerDescriptor[0])) - 1;

// Android 12 native Binder request header: strict-mode policy, work-source UID,
// and vendor header. This is intentionally compile-time because the module is
// fingerprint-locked to one final firmware.
constexpr size_t kBinderHeaderLength = 3 * sizeof(uint32_t);
constexpr size_t kDescriptorOffset = kBinderHeaderLength + sizeof(int32_t);
constexpr size_t kDescriptorStorageBytes =
    (kSessionControllerDescriptorLength + 1) * sizeof(char16_t);
constexpr size_t kMinimumMetadataRequestSize =
    kDescriptorOffset + kDescriptorStorageBytes;
constexpr size_t kNullReplySize = 2 * sizeof(int32_t);
constexpr size_t kMaximumGeneratedReplySize = 4 * 1024;

using TransactFn = int (*)(void*, int32_t, uint32_t, void*, void*, uint32_t);
using ParcelSetDataFn = int32_t (*)(void*, const uint8_t*, size_t);
using ParcelSetDataPositionFn = void (*)(const void*, size_t);

enum InitState : uint32_t {
    kInitNotStarted = 0,
    kInitRunning = 1,
    kInitReady = 2,
    kInitFailed = 3,
};

uint32_t g_init_state = kInitNotStarted;
uint32_t g_get_metadata_code = 0;
TransactFn g_transact_original = nullptr;
ParcelSetDataFn g_parcel_set_data = nullptr;
ParcelSetDataPositionFn g_parcel_set_data_position = nullptr;
void* g_libbinder_handle = nullptr;
uint8_t* g_empty_metadata_reply = nullptr;
size_t g_empty_metadata_reply_size = 0;
uint32_t g_patch_log_once = 0;
uint32_t g_set_data_error_log_once = 0;

COLD bool clearJniException(JNIEnv* env, const char* operation) {
    if (!env->ExceptionCheck()) return false;
    LOGE("JNI exception while %s", operation);
    env->ExceptionClear();
    return true;
}

COLD bool buildEmptyMetadataReply(JNIEnv* env) {
    if (env->PushLocalFrame(16) < 0) {
        clearJniException(env, "creating local JNI frame");
        return false;
    }

    auto finish = [env](bool result) {
        env->PopLocalFrame(nullptr);
        return result;
    };

    jclass parcel_class = env->FindClass("android/os/Parcel");
    jclass builder_class = env->FindClass("android/media/MediaMetadata$Builder");
    if (parcel_class == nullptr || builder_class == nullptr ||
        clearJniException(env, "finding framework classes")) {
        return finish(false);
    }

    jmethodID obtain =
        env->GetStaticMethodID(parcel_class, "obtain", "()Landroid/os/Parcel;");
    jmethodID write_no_exception =
        env->GetMethodID(parcel_class, "writeNoException", "()V");
    jmethodID write_typed_object = env->GetMethodID(
        parcel_class, "writeTypedObject", "(Landroid/os/Parcelable;I)V");
    jmethodID marshall = env->GetMethodID(parcel_class, "marshall", "()[B");
    jmethodID recycle = env->GetMethodID(parcel_class, "recycle", "()V");
    jmethodID builder_constructor =
        env->GetMethodID(builder_class, "<init>", "()V");
    jmethodID build = env->GetMethodID(
        builder_class, "build", "()Landroid/media/MediaMetadata;");

    if (obtain == nullptr || write_no_exception == nullptr ||
        write_typed_object == nullptr || marshall == nullptr || recycle == nullptr ||
        builder_constructor == nullptr || build == nullptr ||
        clearJniException(env, "resolving framework methods")) {
        return finish(false);
    }

    jobject parcel = env->CallStaticObjectMethod(parcel_class, obtain);
    jobject builder = env->NewObject(builder_class, builder_constructor);
    jobject metadata = builder == nullptr ? nullptr : env->CallObjectMethod(builder, build);
    if (parcel == nullptr || builder == nullptr || metadata == nullptr ||
        clearJniException(env, "creating replacement metadata")) {
        return finish(false);
    }

    env->CallVoidMethod(parcel, write_no_exception);
    env->CallVoidMethod(parcel, write_typed_object, metadata, 1);
    if (clearJniException(env, "serializing replacement metadata")) {
        env->CallVoidMethod(parcel, recycle);
        clearJniException(env, "recycling failed Parcel");
        return finish(false);
    }

    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(parcel, marshall));
    if (bytes == nullptr || clearJniException(env, "marshalling replacement reply")) {
        env->CallVoidMethod(parcel, recycle);
        clearJniException(env, "recycling failed Parcel");
        return finish(false);
    }

    const jsize length = env->GetArrayLength(bytes);
    if (length <= static_cast<jsize>(kNullReplySize) ||
        length > static_cast<jsize>(kMaximumGeneratedReplySize)) {
        LOGE("Unexpected replacement Parcel size: %d", length);
        env->CallVoidMethod(parcel, recycle);
        clearJniException(env, "recycling invalid Parcel");
        return finish(false);
    }

    auto* reply = static_cast<uint8_t*>(malloc(static_cast<size_t>(length)));
    if (reply == nullptr) {
        LOGE("Could not allocate %d bytes for replacement Parcel", length);
        env->CallVoidMethod(parcel, recycle);
        clearJniException(env, "recycling allocation-failed Parcel");
        return finish(false);
    }

    env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(reply));
    if (clearJniException(env, "copying replacement Parcel")) {
        free(reply);
        env->CallVoidMethod(parcel, recycle);
        clearJniException(env, "recycling copy-failed Parcel");
        return finish(false);
    }

    int32_t exception_code = -1;
    int32_t presence_marker = -1;
    memcpy(&exception_code, reply, sizeof(exception_code));
    memcpy(&presence_marker, reply + sizeof(exception_code), sizeof(presence_marker));
    if (exception_code != 0 || presence_marker != 1) {
        LOGE("Unexpected replacement Parcel header: exception=%d presence=%d",
             exception_code, presence_marker);
        free(reply);
        env->CallVoidMethod(parcel, recycle);
        clearJniException(env, "recycling header-invalid Parcel");
        return finish(false);
    }

    env->CallVoidMethod(parcel, recycle);
    if (clearJniException(env, "recycling replacement Parcel")) {
        free(reply);
        return finish(false);
    }

    // Intentional process-lifetime allocation. The hook reads this immutable
    // buffer concurrently and Parcel::setData copies it into each reply.
    g_empty_metadata_reply = reply;
    g_empty_metadata_reply_size = static_cast<size_t>(length);
    LOGI("Prepared empty MediaMetadata reply (%zu bytes)", g_empty_metadata_reply_size);
    return finish(true);
}

COLD void* resolveAnySymbol(void* handle, const char* const* names, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (void* symbol = dlsym(RTLD_DEFAULT, names[i]); symbol != nullptr) {
            return symbol;
        }
    }
    if (handle == nullptr) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (void* symbol = dlsym(handle, names[i]); symbol != nullptr) {
            return symbol;
        }
    }
    return nullptr;
}

COLD bool resolveParcelFunctions() {
    if (g_libbinder_handle != nullptr) return true;

#ifdef RTLD_NOLOAD
    constexpr int kDlopenFlags = RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD;
#else
    constexpr int kDlopenFlags = RTLD_NOW | RTLD_LOCAL;
#endif

    // Keep one process-lifetime reference so the resolved native pointers can
    // never outlive the library mapping, even under a nonstandard Zygisk loader.
    g_libbinder_handle = dlopen("libbinder.so", kDlopenFlags);
    if (g_libbinder_handle == nullptr) {
        LOGE("Could not reference loaded libbinder.so: %s", dlerror());
        return false;
    }

#if defined(__LP64__)
    const char* set_data_symbols[] = {
        "_ZN7android6Parcel7setDataEPKhm",
        "_ZN7android6Parcel7setDataEPKvm",
    };
    const char* set_position_symbols[] = {
        "_ZNK7android6Parcel15setDataPositionEm",
        "_ZN7android6Parcel15setDataPositionEm",
    };
#else
    const char* set_data_symbols[] = {
        "_ZN7android6Parcel7setDataEPKhj",
        "_ZN7android6Parcel7setDataEPKvj",
    };
    const char* set_position_symbols[] = {
        "_ZNK7android6Parcel15setDataPositionEj",
        "_ZN7android6Parcel15setDataPositionEj",
    };
#endif

    g_parcel_set_data = reinterpret_cast<ParcelSetDataFn>(resolveAnySymbol(
        g_libbinder_handle, set_data_symbols,
        sizeof(set_data_symbols) / sizeof(set_data_symbols[0])));
    g_parcel_set_data_position = reinterpret_cast<ParcelSetDataPositionFn>(resolveAnySymbol(
        g_libbinder_handle, set_position_symbols,
        sizeof(set_position_symbols) / sizeof(set_position_symbols[0])));

    if (g_parcel_set_data == nullptr || g_parcel_set_data_position == nullptr) {
        LOGE("Could not resolve Parcel::setData/setDataPosition");
        return false;
    }
    return true;
}

ALWAYS_INLINE bool isGetMetadataRequest(const PParcel* request, uint32_t code) {
    if (LIKELY(code != g_get_metadata_code)) return false;
    if (UNLIKELY(request == nullptr || request->data == nullptr ||
                 request->data_size < kMinimumMetadataRequestSize)) {
        return false;
    }

    int32_t descriptor_length = -1;
    memcpy(&descriptor_length, request->data + kBinderHeaderLength,
           sizeof(descriptor_length));
    if (UNLIKELY(descriptor_length !=
                 static_cast<int32_t>(kSessionControllerDescriptorLength))) {
        return false;
    }

    const char* descriptor = request->data + kDescriptorOffset;
    if (UNLIKELY(memcmp(descriptor, kSessionControllerDescriptor,
                        kSessionControllerDescriptorLength * sizeof(char16_t)) != 0)) {
        return false;
    }

    char16_t terminator = 1;
    memcpy(&terminator,
           descriptor + kSessionControllerDescriptorLength * sizeof(char16_t),
           sizeof(terminator));
    return terminator == u'\0';
}

ALWAYS_INLINE bool isNullMetadataReply(const PParcel* reply) {
    if (UNLIKELY(reply == nullptr || reply->data == nullptr ||
                 reply->data_size != kNullReplySize)) {
        return false;
    }

    int32_t exception_code = -1;
    int32_t presence_marker = -1;
    memcpy(&exception_code, reply->data, sizeof(exception_code));
    memcpy(&presence_marker, reply->data + sizeof(exception_code),
           sizeof(presence_marker));
    return exception_code == 0 && presence_marker == 0;
}

HOT int transactHook(void* self, int32_t handle, uint32_t code, void* request,
                     void* reply, uint32_t flags) {
    const bool should_patch =
        UNLIKELY(isGetMetadataRequest(static_cast<PParcel*>(request), code));
    const int result = g_transact_original(self, handle, code, request, reply, flags);

    if (LIKELY(!should_patch || result != 0)) return result;
    if (LIKELY(!isNullMetadataReply(static_cast<PParcel*>(reply)))) return result;

    const int32_t set_data_result = g_parcel_set_data(
        reply, g_empty_metadata_reply, g_empty_metadata_reply_size);
    if (UNLIKELY(set_data_result != 0)) {
        if (__atomic_exchange_n(&g_set_data_error_log_once, 1u,
                                __ATOMIC_RELAXED) == 0) {
            LOGE("Parcel::setData failed: %d", set_data_result);
        }
        return result;
    }

    g_parcel_set_data_position(reply, 0);

    // The load avoids a locked read-modify-write after the first repair.
    if (UNLIKELY(__atomic_load_n(&g_patch_log_once, __ATOMIC_RELAXED) == 0) &&
        __atomic_exchange_n(&g_patch_log_once, 1u, __ATOMIC_RELAXED) == 0) {
        LOGI("Replaced null MediaMetadata Binder reply");
    }
    return result;
}

COLD bool hookBinder(zygisk::Api* api, ino_t inode, dev_t device) {
    api->pltHookRegister(
        device, inode,
        "_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j",
        reinterpret_cast<void*>(&transactHook),
        reinterpret_cast<void**>(&g_transact_original));

    if (!api->pltHookCommit() || g_transact_original == nullptr ||
        g_transact_original == transactHook) {
        LOGE("Could not install a valid IPCThreadState::transact hook");
        return false;
    }
    return true;
}

COLD bool isSupportedBuild() {
    if (android_get_device_api_level() != kSupportedSdk) return false;

    char fingerprint[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.fingerprint", fingerprint) <= 0) return false;
    return strcmp(fingerprint, kSupportedFingerprint) == 0;
}

COLD bool run(zygisk::Api* api, JNIEnv* env) {
    uint32_t expected = kInitNotStarted;
    if (!__atomic_compare_exchange_n(&g_init_state, &expected, kInitRunning,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        if (expected == kInitReady) return true;
        LOGE("Refusing duplicate initialization in state %u", expected);
        return false;
    }

    auto fail = []() {
        __atomic_store_n(&g_init_state, kInitFailed, __ATOMIC_RELEASE);
        return false;
    };

    if (!isSupportedBuild()) {
        LOGE("Unsupported build; refusing to hook");
        return fail();
    }

    ino_t libbinder_inode = 0;
    dev_t libbinder_device = 0;
    if (!getMapping("libbinder.so", &libbinder_inode, &libbinder_device)) {
        LOGE("Could not locate loaded libbinder.so mapping");
        return fail();
    }

    g_get_metadata_code = getStaticIntFieldJni(
        env, STUB("android/media/session/ISessionController"), TRSCTN("getMetadata"));
    if (g_get_metadata_code == 0) {
        LOGE("Could not resolve ISessionController.TRANSACTION_getMetadata");
        return fail();
    }

    if (!buildEmptyMetadataReply(env)) return fail();
    if (!resolveParcelFunctions()) return fail();
    if (!hookBinder(api, libbinder_inode, libbinder_device)) return fail();

    __atomic_store_n(&g_init_state, kInitReady, __ATOMIC_RELEASE);
    LOGI("Hook installed; transaction code=%u", g_get_metadata_code);
    return true;
}

class SystemUIMediaFix final : public zygisk::ModuleBase {
  public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs* args) override {
        (void)args;
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (UNLIKELY(args == nullptr || args->nice_name == nullptr)) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char* process_name = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (UNLIKELY(process_name == nullptr)) {
            clearJniException(env_, "reading process name");
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const bool is_target = strcmp(process_name, kTargetProcess) == 0;
        env_->ReleaseStringUTFChars(args->nice_name, process_name);

        if (!is_target || !run(api_, env_)) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

  private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(SystemUIMediaFix)
