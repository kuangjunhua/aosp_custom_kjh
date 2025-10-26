#define LOG_TAG "LedHidlDemo"

#include <android/hardware/ledhidl/1.0/ILedService.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>  
#include "LedService.h"

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::ledhidl::V1_0::ILedService;
using android::hardware::ledhidl::implementation::LedService;
using namespace android;

status_t registerLedService(){
    sp<ILedService> service = new LedService();
    return service->registerAsService();
}

int main(){
    configureRpcThreadpool(1, true);
    status_t status = registerLedService();
    if(status != OK){
        ALOGD("[Service] register LedService failed");
        return status;
    }
    joinRpcThreadpool();
    ALOGD("LedService return.");
    return 1;
}