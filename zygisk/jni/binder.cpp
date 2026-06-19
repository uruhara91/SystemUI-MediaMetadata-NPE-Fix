#include "binder.hpp"

#include <android/log.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysmacros.h>

#define LOGE(fmt, ...) \
    __android_log_print(ANDROID_LOG_ERROR, "SystemUIMediaFix", "[%d] " fmt, __LINE__, ##__VA_ARGS__)

bool getMapping(const char* library_name, ino_t* inode, dev_t* device) {
    if (library_name == nullptr || inode == nullptr || device == nullptr) return false;

    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    char line[512] = {};
    char permissions[8] = {};
    const size_t library_name_length = strlen(library_name);

    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned int device_major = 0;
        unsigned int device_minor = 0;
        unsigned long parsed_inode = 0;
        int path_offset = 0;

        const int matched = sscanf(line, "%*s %7s %*x %x:%x %lu %*s%n", permissions,
                                   &device_major, &device_minor, &parsed_inode, &path_offset);
        if (matched < 4 || path_offset <= 0 || permissions[2] != 'x') continue;

        const size_t line_length = strlen(line);
        if (line_length < library_name_length) continue;

        char* newline = strchr(line, '\n');
        if (newline != nullptr) *newline = '\0';
        const size_t trimmed_length = strlen(line);
        if (trimmed_length < library_name_length) continue;

        if (memcmp(line + trimmed_length - library_name_length, library_name,
                   library_name_length) != 0) {
            continue;
        }

        *inode = static_cast<ino_t>(parsed_inode);
        *device = makedev(device_major, device_minor);
        fclose(maps);
        return true;
    }

    fclose(maps);
    return false;
}

uint32_t getStaticIntFieldJni(JNIEnv* env, const char* class_name, const char* field_name) {
    jclass target_class = env->FindClass(class_name);
    if (target_class == nullptr) {
        env->ExceptionClear();
        LOGE("Could not find class %s", class_name);
        return 0;
    }

    jfieldID field = env->GetStaticFieldID(target_class, field_name, "I");
    if (field == nullptr) {
        env->ExceptionClear();
        LOGE("Could not find field %s.%s", class_name, field_name);
        env->DeleteLocalRef(target_class);
        return 0;
    }

    const jint value = env->GetStaticIntField(target_class, field);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(target_class);
        LOGE("Could not read field %s.%s", class_name, field_name);
        return 0;
    }

    env->DeleteLocalRef(target_class);
    return static_cast<uint32_t>(value);
}
