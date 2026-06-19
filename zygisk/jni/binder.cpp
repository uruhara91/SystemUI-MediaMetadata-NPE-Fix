#include "binder.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/sysmacros.h>

namespace {

constexpr size_t kMapsLineCapacity = 1024;
constexpr char kDeletedSuffix[] = " (deleted)";

}  // namespace

bool getMapping(const char* library_name, ino_t* inode, dev_t* device) {
    if (library_name == nullptr || inode == nullptr || device == nullptr ||
        library_name[0] == '\0') {
        return false;
    }

    FILE* maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        maps = fopen("/proc/self/maps", "r");
    }
    if (maps == nullptr) return false;

    char line[kMapsLineCapacity] = {};
    char permissions[8] = {};

    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned int device_major = 0;
        unsigned int device_minor = 0;
        unsigned long parsed_inode = 0;
        int path_offset = 0;

        const int matched = sscanf(line, "%*s %7s %*s %x:%x %lu %n",
                                   permissions, &device_major, &device_minor,
                                   &parsed_inode, &path_offset);
        if (matched != 4 || path_offset <= 0 || permissions[2] != 'x' ||
            parsed_inode == 0) {
            continue;
        }

        char* path = line + path_offset;
        while (*path == ' ' || *path == '\t') ++path;
        if (*path == '\0' || *path == '\n' || *path == '[') continue;

        char* newline = strchr(path, '\n');
        if (newline != nullptr) *newline = '\0';

        const size_t path_length = strlen(path);
        constexpr size_t deleted_suffix_length = sizeof(kDeletedSuffix) - 1;
        if (path_length >= deleted_suffix_length &&
            memcmp(path + path_length - deleted_suffix_length, kDeletedSuffix,
                   deleted_suffix_length) == 0) {
            path[path_length - deleted_suffix_length] = '\0';
        }

        const char* base_name = strrchr(path, '/');
        base_name = base_name == nullptr ? path : base_name + 1;
        if (strcmp(base_name, library_name) != 0) continue;

        *inode = static_cast<ino_t>(parsed_inode);
        *device = makedev(device_major, device_minor);
        fclose(maps);
        return true;
    }

    fclose(maps);
    return false;
}
