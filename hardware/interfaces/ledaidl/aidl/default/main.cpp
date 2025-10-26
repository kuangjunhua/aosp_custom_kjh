#define LOG_TAG "LedAidlDemo"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <stdio.h>
#include <log/log.h>
#include <iostream>


#include "LedManagerImpl.h"

using aidl::android::hardware::ledaidl::LedManagerImpl;

int main(){
    //0表示单线程串行处理请求
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    std::shared_ptr<LedManagerImpl> service = ::ndk::SharedRefBase::make<LedManagerImpl>();

    //这段代码的作用是 拼接一个 Binder 服务的注册名称，格式通常为：<接口描述符>/default
    //android.hardware.ledaidl.ILedManager/default
    const std::string servicename = std::string() + LedManagerImpl::descriptor + "/default";
    ALOGD("[Service] servicename = %s \n", servicename.c_str());
    // [Service] servicename = android.hardware.ledaidl.ILedManager/default out
    if(service == nullptr){
        ALOGD("[Service] service is nullptr");
    }else{
        ALOGD("[Service] service is not nullptr"); // out
    }
    auto binder = service->asBinder();
    if(binder == nullptr){
        ALOGD("[Service] binder is nullptr");
    }else{
        if(binder.get() == nullptr){
            ALOGD("[Service] binder.get() is nullptr");
        }else{
            ALOGD("[Service] binder.get() not nullptr"); // out
        }
    }
    //添加binder服务到sm
    binder_status_t status = AServiceManager_addService(service->asBinder().get(), servicename.c_str());
    ALOGD("[Service] status = %d", status);
    if(status != STATUS_OK) {
        ALOGD("[Service] AServiceManager_addService failed \n");
    }else{
        ALOGD("[Service] AServiceManager_addService success \n");
    }
    ABinderProcess_joinThreadPool();
    return 0;
}