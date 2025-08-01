#ifndef _HARDWARE_TEMPERATURE_H
#define _HARDWARE_TEMPERATURE_H

#include <hardware/hardware.h>

/**
 * The id of this module
 */
#define TEMPERATURE_HARDWARE_MODULE_ID "temperature"

struct temperature_module_t {
	struct hw_module_t common;	
};
 
struct temperature_device_t {
	struct hw_device_t common;
	int (*read_temperature)();
};

#endif  // _HARDWARE_VIBRATOR_H
