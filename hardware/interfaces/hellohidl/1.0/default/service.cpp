#define LOG_TAG "android.hardware.hellohidl@1.0-service"

#include <android/hardware/hellohidl/1.0/IMyService.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>  
#include "MyService.h"

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::hellohidl::V1_0::IMyService;
using android::hardware::hellohidl::implementation::MyService;
using namespace android;

status_t registerMyService(){
    sp<IMyService> service = new MyService();
    return service->registerAsService(); // register service into hwservicemanager
}
int main(){
    configureRpcThreadpool(1, true);
    status_t status = registerMyService();

    if(status != OK){
        ALOGE("register MyService not success.");
        return status;
    }

    joinRpcThreadpool();
    ALOGD("MyService return.");
    return 1;
}