#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Minimal prefix of android::Parcel for the fingerprint-locked Android 12
// arm64 target. The first machine word contains state/error fields, followed
// by mData and mDataSize.
struct PParcel {
    size_t state;
    char* data;
    size_t data_size;
};

static_assert(sizeof(void*) == 8, "This module is intentionally arm64-only");
static_assert(offsetof(PParcel, data) == sizeof(size_t),
              "Unexpected PParcel::data offset");
static_assert(offsetof(PParcel, data_size) == 2 * sizeof(size_t),
              "Unexpected PParcel::data_size offset");
static_assert(sizeof(PParcel) == 3 * sizeof(size_t),
              "Unexpected PParcel prefix size");

bool getMapping(const char* library_name, ino_t* inode, dev_t* device);
