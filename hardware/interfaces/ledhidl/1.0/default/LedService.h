// FIXME: your file license if you have one

#pragma once

#include <android/hardware/ledhidl/1.0/ILedService.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

namespace android::hardware::ledhidl::implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

struct LedService : public V1_0::ILedService {
    public:
        int32_t mStatus = 0;
    
    // Methods from ::android::hardware::ledhidl::V1_0::ILedService follow.
    Return<int32_t> getStatus() override;
    Return<void> operate(int32_t newStatus, operate_cb _hidl_cb) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.

};

// FIXME: most likely delete, this is only for passthrough implementations
// extern "C" ILedService* HIDL_FETCH_ILedService(const char* name);

}  // namespace android::hardware::ledhidl::implementation
