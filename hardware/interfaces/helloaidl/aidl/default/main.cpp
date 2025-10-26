#define LOG_TAG "DeviceStatusManagerImpl"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <stdio.h>
#include <log/log.h>
#include <iostream>


#include "DeviceStatusManagerImpl.h"


using aidl::android::hardware::helloaidl::DeviceStatusManagerImpl;

int main(){
    //0表示单线程串行处理请求
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    std::shared_ptr<DeviceStatusManagerImpl> service = ::ndk::SharedRefBase::make<DeviceStatusManagerImpl>();

    //这段代码的作用是 拼接一个 Binder 服务的注册名称，格式通常为：<接口描述符>/default
    //android.hardware.helloaidl.IDeviceStatusManager/default
    const std::string servicename = std::string() + DeviceStatusManagerImpl::descriptor + "/default";
    ALOGD("DeviceStatusManager servicename = %s \n", servicename.c_str());

    //添加binder服务到sm
    binder_status_t status = AServiceManager_addService(service->asBinder().get(), servicename.c_str());
    if(status != STATUS_OK) {
        ALOGD("AServiceManager_addService failed \n");
    }else{
        ALOGD("AServiceManager_addService success \n");
    }
    ABinderProcess_joinThreadPool();
    return 0;
}