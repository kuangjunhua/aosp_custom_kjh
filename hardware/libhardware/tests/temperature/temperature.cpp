#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include <log/log.h>
#include <utils/Timers.h>

#include <hardware/temperature.h>

int main(int /* argc */, char** /* argv */)
{

	int err;
	struct temperature_module_t* module = NULL;
	struct temperature_device_t* device = NULL;

	// get module
	err = hw_get_module(TEMPERATURE_HARDWARE_MODULE_ID, (hw_module_t const**)&module);
	if(err != 0){
		printf("hw_get_module() failed (%s)\n", strerror(-err));
		return 0;
	}

	// get device
	err = module->common.methods->open(&module->common, NULL, (hw_device_t**)&device);
	if(err != 0){
		printf("open() failed (%s)\n", strerror(-err));
		return 0;
	}

	// operate the device:temperature driver
	int value = device->read_temperature();
	printf("temperature = %d\n", value);

	// close the device
	err = device->common.close((hw_device_t*)device);
	if(err != 0){
		printf("close() failed (%s)\n", strerror(-err));
	}

    return 0;
}
