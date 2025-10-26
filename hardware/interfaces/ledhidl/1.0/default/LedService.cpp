// FIXME: your file license if you have one

#include "LedService.h"
#include <log/log.h>

#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <utils/Timers.h>
#include <hardware/led.h>

namespace android::hardware::ledhidl::implementation {

    void callLedHal(){
        int err;
        struct led_module_t* ledModule = NULL;
        struct led_device_t* ledDevice = NULL;

        err = hw_get_module(LED_HARDWARE_MODULE_ID,(hw_module_t const**)&ledModule);
        if(err != 0){
            ALOGD("hw_get_module() failed (%s)\n", strerror(-err));
        }
        err = ledModule->module.methods->open(&ledModule->module, NULL, (hw_device_t**)&ledDevice);
        if(err != 0){
            ALOGD("open() failed (%s)\n", strerror(-err));
        }
        int ledStatus = ledDevice->get_status();
        ALOGD("1: status=%d\n", ledStatus); // 0
        // open
        ledDevice->open_led();
        ledStatus = ledDevice->get_status();
        ALOGD("2: status=%d\n", ledStatus); // 1
        // close
        ledDevice->close_led();
        ledStatus = ledDevice->get_status();
        ALOGD("2: status=%d\n", ledStatus); // 0
    }

// Methods from ::android::hardware::ledhidl::V1_0::ILedService follow.
Return<int32_t> LedService::getStatus() {
    callLedHal();
    return mStatus;
}

Return<void> LedService::operate(int32_t newStatus, operate_cb _hidl_cb) {
    mStatus = newStatus;
    _hidl_cb(newStatus, "status updated");
    return Void();
}


// Methods from ::android::hidl::base::V1_0::IBase follow.

//ILedService* HIDL_FETCH_ILedService(const char* /* name */) {
    //return new LedService();
//}
//
}  // namespace android::hardware::ledhidl::implementation
