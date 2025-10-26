#define LOG_TAG "Client-StableAidl"

#include <iostream>
#include <log/log.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <aidl/android/hardware/helloaidl/IDeviceStatusManager.h>
#include <aidl/android/hardware/helloaidl/BnDeviceStatusListener.h>
#include <aidl/android/hardware/helloaidl/StatusSetting.h>

using aidl::android::hardware::helloaidl::IDeviceStatusManager;
using aidl::android::hardware::helloaidl::BnDeviceStatusListener;
using aidl::android::hardware::helloaidl::StatusSetting;

// 实现监听器接口
class HalListener : public BnDeviceStatusListener {
public:
        ndk::ScopedAStatus onStatusChanged(int32_t status, const std::string& msg) {
            ALOGI("Received status change: %d, msg:%s", status, msg.c_str());
            // 在这里省略处理状态变化
            return ndk::ScopedAStatus::ok();
    }

};

int main(){
        ALOGD("Entry...");
        ABinderProcess_setThreadPoolMaxThreadCount(0); //启动binder线程池
        ABinderProcess_startThreadPool(); //必须启动，否则服务调调回调时无法处理上面的onStatusChanged接口

        ndk::SpAIBinder binder(AServiceManager_waitForService("android.hardware.helloaidl.IDeviceStatusManager/default"));
        std::shared_ptr<IDeviceStatusManager> service = IDeviceStatusManager::fromBinder(binder); //from binder to Interface.

        if( service == nullptr ){
                ALOGE("get service fail...");
                return -1;
        }

        auto listener = ndk::SharedRefBase::make<HalListener>();

        //跨进程调用注册接口
        ndk::ScopedAStatus status = service->registerListener(listener);
        if( !status.isOk() ){
                ALOGE("Failed to register listener: %s", status.getDescription().c_str());
                return -1;
        }

        ALOGI("Listener registered successfully!");


        sleep(3);
        ALOGD("Client is alive...");

        //跨进程调用设置状态
        StatusSetting setting{"Light1", 0};
        service->setStatus(setting);

        //clear.
        setting.devicename="none";
        setting.val = 0;

        //跨进程调用获取状态
        service->getCurrentStatus(&setting);
        ALOGD("get status: devicename:%s, val=%d", setting.devicename.c_str(), setting.val);

        //跨进程调用注销接口
        status = service->unregisterListener(listener);
        if( !status.isOk() ){
                ALOGE("Failed to register listener: %s", status.getDescription().c_str());
                return -1;
        }

        sleep(3);
        return 0;
}