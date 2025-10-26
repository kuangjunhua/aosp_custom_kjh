#ifndef ANDROID_HARDWARE_LEDMANAGERIMPL_H_
#define ANDROID_HARDWARE_LEDMANAGERIMPL_H_

#include <aidl/android/hardware/ledaidl/BnLedManager.h>
#include <iostream>

namespace aidl::android::hardware::ledaidl {
    class LedManagerImpl : public BnLedManager {
        private:
            int32_t mStatus = 0; // 0: closed 1: opened
        public:
            ::ndk::ScopedAStatus getStatus(int32_t* _aidl_return) override;
            ::ndk::ScopedAStatus open() override;
            ::ndk::ScopedAStatus close() override;
    };
}

#endif