#define LOG_TAG "LedAidlDemo"

#include <iostream>
#include <log/log.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <aidl/android/hardware/ledaidl/ILedManager.h>


using aidl::android::hardware::ledaidl::ILedManager;

int main(){
    ALOGD("Entry...");
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    ABinderProcess_startThreadPool();

    ndk::SpAIBinder binder(AServiceManager_waitForService("android.hardware.ledaidl.ILedManager/default"));
    std::shared_ptr<ILedManager> service = ILedManager::fromBinder(binder);

    if(service == nullptr){
        ALOGD("[Client] get service failed !!!");
        return -1;
    }else{
        ALOGI("[Client] get service success...");
    }

    ALOGD("[Client] start get status");

    int32_t status;
    service->getStatus(&status);
    ALOGD("[Client] 1 status=%d", status);
    sleep(2);

    ALOGD("[Client] open~~~");
    service->open();
    service->getStatus(&status);
    ALOGD("[Client] 2 status=%d", status);
    sleep(2);

    ALOGD("[Client] close~~~");
    service->close();
    service->getStatus(&status);
    ALOGD("[Client] 3 status=%d", status);

    return 0;
}