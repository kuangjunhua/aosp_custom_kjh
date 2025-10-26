// FIXME: your file license if you have one

#pragma once

#include <android/hardware/hellohidl/1.0/IMyService.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

namespace android::hardware::hellohidl::implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

struct MyService : public V1_0::IMyService {
    // Methods from ::android::hardware::hellohidl::V1_0::IMyService follow.
    Return<void> exampleAdd(int32_t a, int32_t b, exampleAdd_cb _hidl_cb) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.

};

// FIXME: most likely delete, this is only for passthrough implementations
// extern "C" IMyService* HIDL_FETCH_IMyService(const char* name);

}  // namespace android::hardware::hellohidl::implementation
