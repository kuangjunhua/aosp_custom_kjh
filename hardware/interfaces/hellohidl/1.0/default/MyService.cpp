// FIXME: your file license if you have one

#include "MyService.h"
#include <log/log.h>

#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include <hardware/temperature.h>

namespace android::hardware::hellohidl::implementation {

    int CallhalModule(void){
        ALOGD("Start HAL");
        int err;
        struct temperature_module_t* module = NULL;
        struct temperature_device_t* device = NULL;

        // get module
        err = hw_get_module(TEMPERATURE_HARDWARE_MODULE_ID, (hw_module_t const**)&module);
        if(err != 0){
            ALOGD("hw_get_module() failed (%s)\n", strerror(-err));
            return 0;
        }

        // get device
        err = module->common.methods->open(&module->common, NULL, (hw_device_t**)&device);
        if(err != 0){
            ALOGD("open() failed (%s)\n", strerror(-err));
            return 0;
        }

        // operate the device:temperature driver
        int value = device->read_temperature();
        ALOGD("called by hidl ipc temperature = %d\n", value);

        // close the device
        err = device->common.close((hw_device_t*)device);
        if(err != 0){
            ALOGD("close() failed (%s)\n", strerror(-err));
        }

        return 0;
    }

// Methods from ::android::hardware::hellohidl::V1_0::IMyService follow.
Return<void> MyService::exampleAdd(int32_t a, int32_t b, exampleAdd_cb _hidl_cb) {
    int32_t ret = a + b;
    _hidl_cb(ret, "this is the result"); // call cb to get result

    ALOGD("this is hidl api: exampleAdd: a=%d, b=%d, ret=%d", a, b, ret);

    CallhalModule(); //在这里调用hal module

    return Void();
}


// Methods from ::android::hidl::base::V1_0::IBase follow.

//IMyService* HIDL_FETCH_IMyService(const char* /* name */) {
    //return new MyService();
//}
//
}  // namespace android::hardware::hellohidl::implementation
