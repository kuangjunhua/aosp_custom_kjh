/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <hardware/hardware.h>
#include <hardware/temperature.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>
 
int temp_close(struct hw_device_t* device){
	free(device);
	// other release...
	return 0;
}

int read_temp_impl(){
	int a;

	/*
	 * real sence step
	 * 1. open driver
	 * 2. ioctl setting driver
	 * 3. read data from driver
	 */

	a = 25;
	return a;
}

int temp_device_open(const struct hw_module_t* module, const char* id __unused,struct hw_device_t** device){
	struct temperature_device_t *dev = (struct temperature_device_t*) malloc(sizeof(struct temperature_device_t));
	memset(dev, 0, sizeof(struct temperature_device_t));
	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (hw_module_t*)module;

	dev->common.close = temp_close;

	dev->read_temperature = read_temp_impl;
	*device = &dev->common;

	return 0;
}

/* Default vibrator HW module interface definition                           */

static struct hw_module_methods_t temp_methods = {
    .open = temp_device_open,
};

struct temperature_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.id = TEMPERATURE_HARDWARE_MODULE_ID,
		.name = "Default temperature HAL",
		.methods = &temp_methods, // generate a temp device
	}
};
