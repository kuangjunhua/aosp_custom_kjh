#ifndef ANDROID_HARDWARE_DEVICESTATUSMANAGERIMPL_H_
#define ANDROID_HARDWARE_DEVICESTATUSMANAGERIMPL_H_

#include <aidl/android/hardware/helloaidl/BnDeviceStatusManager.h>
#include <aidl/android/hardware/helloaidl/IDeviceStatusListener.h>
#include <aidl/android/hardware/helloaidl/StatusSetting.h>
#include <iostream>

namespace aidl::android::hardware::helloaidl {
    class DeviceStatusManagerImpl : public BnDeviceStatusManager {
        private:
            // 存储客户端
            std::vector<std::shared_ptr<IDeviceStatusListener>> mListeners;
            StatusSetting mStatus = { "Light", 1 };
        public:
            void updateStatus(int32_t code, const std::string& msg);
            std::vector<std::shared_ptr<IDeviceStatusListener>> getListeners(void);

            ::ndk::ScopedAStatus registerListener(const std::shared_ptr<::aidl::android::hardware::helloaidl::IDeviceStatusListener>& in_listener) override;
            ::ndk::ScopedAStatus unregisterListener(const std::shared_ptr<::aidl::android::hardware::helloaidl::IDeviceStatusListener>& in_listener) override;
            ::ndk::ScopedAStatus getCurrentStatus(::aidl::android::hardware::helloaidl::StatusSetting* _aidl_return) override;
            ::ndk::ScopedAStatus setStatus(const ::aidl::android::hardware::helloaidl::StatusSetting& in_s) override;
    };
}

#endif  // ANDROID_HARDWARE_DEVICESTATUSMANAGERIMPL_H_