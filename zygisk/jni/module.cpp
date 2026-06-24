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

#ifndef SYSTEMUI_MEDIA_FIX_INFO_LOGS
#define SYSTEMUI_MEDIA_FIX_INFO_LOGS 0
#endif

#if SYSTEMUI_MEDIA_FIX_INFO_LOGS
#define LOGI(fmt, ...) \
    __android_log_print(ANDROID_LOG_INFO, "SystemUIMediaFix", "[%d] " fmt, \
                        __LINE__ __VA_OPT__(,) __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif
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
constexpr char16_t kTargetProcess[] = u"com.android.systemui";
constexpr size_t kTargetProcessLength =
    (sizeof(kTargetProcess) / sizeof(kTargetProcess[0])) - 1;
constexpr char kSupportedFingerprint[] =
    "Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys";
constexpr char16_t kSessionControllerDescriptor[] =
    u"android.media.session.ISessionController";
constexpr size_t kSessionControllerDescriptorLength =
    (sizeof(kSessionControllerDescriptor) / sizeof(kSessionControllerDescriptor[0])) - 1;

// Android 12 ISessionController.aidl: getMetadata is transaction 32.
// The framework, SystemUI and libbinder are fixed by the locked final firmware.
constexpr uint32_t kGetMetadataTransactionCode = 32;

// Android 12 native Binder request header: strict-mode policy, work-source UID,
// and vendor header. These offsets are intentionally compile-time for this
// single fingerprint-locked firmware.
constexpr size_t kBinderHeaderLength = 3 * sizeof(uint32_t);
constexpr size_t kDescriptorOffset = kBinderHeaderLength + sizeof(int32_t);
constexpr size_t kDescriptorStorageBytes =
    (kSessionControllerDescriptorLength + 1) * sizeof(char16_t);
constexpr size_t kMinimumMetadataRequestSize =
    kDescriptorOffset + kDescriptorStorageBytes;
constexpr size_t kNullReplySize = sizeof(uint64_t);
constexpr size_t kMaximumGeneratedReplySize = 4 * 1024;

constexpr char kTransactSymbol[] =
    "_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j";

using TransactFn = int (*)(void*, int32_t, uint32_t, void*, void*, uint32_t);
using ParcelSetDataFn = int32_t (*)(void*, const uint8_t*, size_t);
using ParcelSetDataPositionFn = void (*)(const void*, size_t);

enum InitState : uint32_t {
    kInitNotStarted = 0,
    kInitRunning = 1,
    kInitReady = 2,
    kInitFailed = 3,
};

struct alignas(64) HotState {
    TransactFn transact_original = nullptr;
    uint8_t* empty_metadata_reply = nullptr;
    size_t empty_metadata_reply_size = 0;
} g_hot;

struct alignas(64) ColdState {
    uint32_t init_state = kInitNotStarted;
    ParcelSetDataFn parcel_set_data = nullptr;
    ParcelSetDataPositionFn parcel_set_data_position = nullptr;
    void* libbinder_handle = nullptr;
    uint32_t set_data_error_log_once = 0;
#if SYSTEMUI_MEDIA_FIX_INFO_LOGS
    uint32_t patch_log_once = 0;
#endif
} g_cold;

COLD bool clearJniException(JNIEnv* env, const char* operation) {
    if (!env->ExceptionCheck()) return false;
    LOGE("JNI exception while %s", operation);
    env->ExceptionClear();
    return true;
}

COLD bool recycleParcel(JNIEnv* env, jobject parcel, jmethodID recycle,
                        const char* operation) {
    if (parcel == nullptr || recycle == nullptr) return true;
    env->CallVoidMethod(parcel, recycle);
    return !clearJniException(env, operation);
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
    if (clearJniException(env, "finding android.os.Parcel") ||
        parcel_class == nullptr) {
        return finish(false);
    }

    jclass builder_class = env->FindClass("android/media/MediaMetadata$Builder");
    if (clearJniException(env, "finding MediaMetadata.Builder") ||
        builder_class == nullptr) {
        return finish(false);
    }

    jmethodID obtain =
        env->GetStaticMethodID(parcel_class, "obtain", "()Landroid/os/Parcel;");
    if (clearJniException(env, "resolving Parcel.obtain") || obtain == nullptr) {
        return finish(false);
    }

    jmethodID write_no_exception =
        env->GetMethodID(parcel_class, "writeNoException", "()V");
    if (clearJniException(env, "resolving Parcel.writeNoException") ||
        write_no_exception == nullptr) {
        return finish(false);
    }

    jmethodID write_typed_object = env->GetMethodID(
        parcel_class, "writeTypedObject", "(Landroid/os/Parcelable;I)V");
    if (clearJniException(env, "resolving Parcel.writeTypedObject") ||
        write_typed_object == nullptr) {
        return finish(false);
    }

    jmethodID marshall = env->GetMethodID(parcel_class, "marshall", "()[B");
    if (clearJniException(env, "resolving Parcel.marshall") || marshall == nullptr) {
        return finish(false);
    }

    jmethodID recycle = env->GetMethodID(parcel_class, "recycle", "()V");
    if (clearJniException(env, "resolving Parcel.recycle") || recycle == nullptr) {
        return finish(false);
    }

    jmethodID builder_constructor =
        env->GetMethodID(builder_class, "<init>", "()V");
    if (clearJniException(env, "resolving MediaMetadata.Builder constructor") ||
        builder_constructor == nullptr) {
        return finish(false);
    }

    jmethodID build = env->GetMethodID(
        builder_class, "build", "()Landroid/media/MediaMetadata;");
    if (clearJniException(env, "resolving MediaMetadata.Builder.build") ||
        build == nullptr) {
        return finish(false);
    }

    jobject parcel = env->CallStaticObjectMethod(parcel_class, obtain);
    if (clearJniException(env, "obtaining replacement Parcel") || parcel == nullptr) {
        return finish(false);
    }

    jobject builder = env->NewObject(builder_class, builder_constructor);
    if (clearJniException(env, "creating MediaMetadata.Builder") || builder == nullptr) {
        recycleParcel(env, parcel, recycle, "recycling builder-failed Parcel");
        return finish(false);
    }

    jobject metadata = env->CallObjectMethod(builder, build);
    if (clearJniException(env, "building empty MediaMetadata") || metadata == nullptr) {
        recycleParcel(env, parcel, recycle, "recycling metadata-failed Parcel");
        return finish(false);
    }

    env->CallVoidMethod(parcel, write_no_exception);
    if (clearJniException(env, "writing no-exception header")) {
        recycleParcel(env, parcel, recycle, "recycling header-failed Parcel");
        return finish(false);
    }

    env->CallVoidMethod(parcel, write_typed_object, metadata, 1);
    if (clearJniException(env, "serializing empty MediaMetadata")) {
        recycleParcel(env, parcel, recycle, "recycling serialization-failed Parcel");
        return finish(false);
    }

    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(parcel, marshall));
    if (clearJniException(env, "marshalling replacement reply") || bytes == nullptr) {
        recycleParcel(env, parcel, recycle, "recycling marshalling-failed Parcel");
        return finish(false);
    }

    const jsize length = env->GetArrayLength(bytes);
    if (clearJniException(env, "reading replacement Parcel size") ||
        length <= static_cast<jsize>(kNullReplySize) ||
        length > static_cast<jsize>(kMaximumGeneratedReplySize)) {
        LOGE("Unexpected replacement Parcel size: %d", length);
        recycleParcel(env, parcel, recycle, "recycling size-invalid Parcel");
        return finish(false);
    }

    auto* reply = static_cast<uint8_t*>(malloc(static_cast<size_t>(length)));
    if (reply == nullptr) {
        LOGE("Could not allocate %d bytes for replacement Parcel", length);
        recycleParcel(env, parcel, recycle, "recycling allocation-failed Parcel");
        return finish(false);
    }

    env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(reply));
    if (clearJniException(env, "copying replacement Parcel")) {
        free(reply);
        recycleParcel(env, parcel, recycle, "recycling copy-failed Parcel");
        return finish(false);
    }

    uint64_t reply_header = 0;
    memcpy(&reply_header, reply, sizeof(reply_header));
    if (reply_header != (uint64_t{1} << 32)) {
        int32_t exception_code = -1;
        int32_t presence_marker = -1;
        memcpy(&exception_code, reply, sizeof(exception_code));
        memcpy(&presence_marker, reply + sizeof(exception_code),
               sizeof(presence_marker));
        LOGE("Unexpected replacement Parcel header: exception=%d presence=%d",
             exception_code, presence_marker);
        free(reply);
        recycleParcel(env, parcel, recycle, "recycling header-invalid Parcel");
        return finish(false);
    }

    if (!recycleParcel(env, parcel, recycle, "recycling replacement Parcel")) {
        free(reply);
        return finish(false);
    }

    // Intentional process-lifetime allocation. The Binder hook concurrently
    // reads this immutable buffer and Parcel::setData copies it into each reply.
    g_hot.empty_metadata_reply = reply;
    g_hot.empty_metadata_reply_size = static_cast<size_t>(length);
    LOGI("Prepared empty MediaMetadata reply (%zu bytes)", g_hot.empty_metadata_reply_size);
    return finish(true);
}

COLD void* resolveAnySymbol(void* handle, const char* const* names, size_t count) {
    if (handle != nullptr) {
        for (size_t i = 0; i < count; ++i) {
            if (void* symbol = dlsym(handle, names[i]); symbol != nullptr) {
                return symbol;
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        if (void* symbol = dlsym(RTLD_DEFAULT, names[i]); symbol != nullptr) {
            return symbol;
        }
    }
    return nullptr;
}

COLD void resetBinderSymbols() {
    g_hot.transact_original = nullptr;
    g_cold.parcel_set_data = nullptr;
    g_cold.parcel_set_data_position = nullptr;
    if (g_cold.libbinder_handle != nullptr) {
        dlclose(g_cold.libbinder_handle);
        g_cold.libbinder_handle = nullptr;
    }
}

COLD bool resolveBinderFunctions() {
    if (g_cold.libbinder_handle != nullptr) {
        return g_hot.transact_original != nullptr && g_cold.parcel_set_data != nullptr &&
               g_cold.parcel_set_data_position != nullptr;
    }

#ifdef RTLD_NOLOAD
    g_cold.libbinder_handle =
        dlopen("libbinder.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
#endif
    if (g_cold.libbinder_handle == nullptr) {
        g_cold.libbinder_handle = dlopen("libbinder.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (g_cold.libbinder_handle == nullptr) {
        LOGE("Could not reference libbinder.so: %s", dlerror());
        return false;
    }

    const char* set_data_symbols[] = {
        "_ZN7android6Parcel7setDataEPKhm",
        "_ZN7android6Parcel7setDataEPKvm",
    };
    const char* set_position_symbols[] = {
        "_ZNK7android6Parcel15setDataPositionEm",
        "_ZN7android6Parcel15setDataPositionEm",
    };
    const char* transact_symbols[] = {kTransactSymbol};

    g_cold.parcel_set_data = reinterpret_cast<ParcelSetDataFn>(resolveAnySymbol(
        g_cold.libbinder_handle, set_data_symbols,
        sizeof(set_data_symbols) / sizeof(set_data_symbols[0])));
    g_cold.parcel_set_data_position = reinterpret_cast<ParcelSetDataPositionFn>(
        resolveAnySymbol(g_cold.libbinder_handle, set_position_symbols,
                         sizeof(set_position_symbols) /
                             sizeof(set_position_symbols[0])));
    g_hot.transact_original = reinterpret_cast<TransactFn>(resolveAnySymbol(
        g_cold.libbinder_handle, transact_symbols,
        sizeof(transact_symbols) / sizeof(transact_symbols[0])));

    if (g_cold.parcel_set_data == nullptr || g_cold.parcel_set_data_position == nullptr ||
        g_hot.transact_original == nullptr) {
        LOGE("Could not resolve required libbinder functions");
        resetBinderSymbols();
        return false;
    }
    return true;
}

ALWAYS_INLINE bool isExpectedMetadataRequest(const PParcel* request) {
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

    uint64_t header = ~uint64_t{0};
    memcpy(&header, reply->data, sizeof(header));
    return header == 0;
}

HOT int transactHook(void* self, int32_t handle, uint32_t code, void* request,
                     void* reply, uint32_t flags) {
    if (LIKELY(code != kGetMetadataTransactionCode)) {
        return g_hot.transact_original(self, handle, code, request, reply, flags);
    }

    const int result =
        g_hot.transact_original(self, handle, code, request, reply, flags);
    if (LIKELY(result != 0)) return result;
    if (LIKELY(!isNullMetadataReply(static_cast<const PParcel*>(reply)))) {
        return result;
    }

    // Descriptor parsing is deliberately delayed until a successful reply has
    // the exact nullable-object-null shape. This keeps false-positive
    // transaction-code collisions off the hot path.
    if (LIKELY(!isExpectedMetadataRequest(static_cast<const PParcel*>(request)))) {
        return result;
    }

    const int32_t set_data_result = g_cold.parcel_set_data(
        reply, g_hot.empty_metadata_reply, g_hot.empty_metadata_reply_size);
    if (UNLIKELY(set_data_result != 0)) {
        if (__atomic_exchange_n(&g_cold.set_data_error_log_once, 1u, __ATOMIC_RELEASE) == 0) {
            LOGE("Parcel::setData failed: %d", set_data_result);
        }
        return result;
    }

    g_cold.parcel_set_data_position(reply, 0);

#if SYSTEMUI_MEDIA_FIX_INFO_LOGS
    if (UNLIKELY(__atomic_load_n(&g_cold.patch_log_once, __ATOMIC_RELAXED) == 0) &&
        __atomic_exchange_n(&g_cold.patch_log_once, 1u, __ATOMIC_RELAXED) == 0) {
        LOGI("Replaced null MediaMetadata Binder reply");
    }
#endif
    return result;
}

COLD bool hookBinder(zygisk::Api* api, ino_t inode, dev_t device) {
    api->pltHookRegister(device, inode, kTransactSymbol,
                         reinterpret_cast<void*>(&transactHook),
                         reinterpret_cast<void**>(&g_hot.transact_original));

    const TransactFn fallback = g_hot.transact_original;
    const bool committed = api->pltHookCommit();
    if (g_hot.transact_original == nullptr || g_hot.transact_original == transactHook) {
        g_hot.transact_original = fallback;
    }

    if (!committed || g_hot.transact_original == nullptr ||
        g_hot.transact_original == transactHook) {
        LOGE("Could not install a valid IPCThreadState::transact hook");
        // Self-referential or null hook is unrecoverable — crash rather than
        // silently forward all Binder calls into an infinite loop.
        if (g_hot.transact_original == transactHook) __builtin_trap();
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
    if (!__atomic_compare_exchange_n(&g_cold.init_state, &expected, kInitRunning,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        if (expected == kInitReady) return true;
        LOGE("Refusing duplicate initialization in state %u", expected);
        return false;
    }

    auto fail = []() {
        __atomic_store_n(&g_cold.init_state, kInitFailed, __ATOMIC_RELEASE);
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

    if (!buildEmptyMetadataReply(env)) return fail();
    if (!resolveBinderFunctions()) return fail();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!hookBinder(api, libbinder_inode, libbinder_device)) return fail();

    __atomic_store_n(&g_cold.init_state, kInitReady, __ATOMIC_RELEASE);
    LOGI("Hook installed; transaction code=%u", kGetMetadataTransactionCode);
    return true;
}

COLD bool isTargetProcess(JNIEnv* env, jstring process_name) {
    if (process_name == nullptr) return false;

    const jsize length = env->GetStringLength(process_name);
    if (clearJniException(env, "reading process-name length") ||
        length != static_cast<jsize>(kTargetProcessLength)) {
        return false;
    }

    static_assert(sizeof(jchar) == sizeof(char16_t));
    jchar buffer[kTargetProcessLength];
    env->GetStringRegion(process_name, 0,
                         static_cast<jsize>(kTargetProcessLength), buffer);
    if (clearJniException(env, "reading process name")) return false;

    return memcmp(buffer, kTargetProcess, sizeof(buffer)) == 0;
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
        if (UNLIKELY(args == nullptr || args->nice_name == nullptr ||
                     !isTargetProcess(env_, args->nice_name))) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Never request DLCLOSE for the target. If a loader partially commits
        // a PLT hook before reporting failure, keeping this library mapped
        // prevents a dangling hook into unmapped code.
        (void)run(api_, env_);
    }

  private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(SystemUIMediaFix)

}
