/***************************************************************************//**
 *   @file   linux/linux_gpio.c
 *   @brief  Implementation of Linux platform GPIO Driver.
 *   @author Dragos Bogdan (dragos.bogdan@analog.com)
********************************************************************************
 * Copyright 2020(c) Analog Devices, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Analog Devices, Inc. nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES, INC. “AS IS” AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL ANALOG DEVICES, INC. BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include "no_os_error.h"
#include "no_os_gpio.h"
#include "no_os_delay.h"
// #include "no_os_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/**
 * @struct linux_gpio_desc
 * @brief Linux platform specific GPIO descriptor
 */
struct linux_gpio_desc {
	int line_fd;
	unsigned int offset;
	uint8_t direction;
	uint8_t has_direction;
	char chip_path[64];
};

#define GPIO_MAX_CHIPS 32
#define GPIO_DEFAULT_CHIP_HINT "zynq_gpio"

static int linux_gpio_open_chip_for_offset(unsigned int offset, char *chip_path,
					   size_t chip_path_size)
{
	const char *override_path = getenv("NO_OS_GPIOCHIP");
	int fallback_fd = -1;
	char fallback_path[64] = {0};
	int idx;

	if (override_path == NULL || override_path[0] == '\0')
		override_path = getenv("E200_GPIOCHIP");

	if (override_path != NULL && override_path[0] != '\0') {
		int fd = open(override_path, O_RDONLY);
		if (fd < 0)
			return -errno;
		snprintf(chip_path, chip_path_size, "%s", override_path);
		return fd;
	}

	for (idx = 0; idx < GPIO_MAX_CHIPS; ++idx) {
		char path[64];
		struct gpiochip_info info = {0};
		int fd;

		snprintf(path, sizeof(path), "/dev/gpiochip%d", idx);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;

		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
			close(fd);
			continue;
		}

		if ((unsigned int)info.lines <= offset) {
			close(fd);
			continue;
		}

		if (strstr(info.label, GPIO_DEFAULT_CHIP_HINT) != NULL ||
		    strstr(info.name, GPIO_DEFAULT_CHIP_HINT) != NULL) {
			snprintf(chip_path, chip_path_size, "%s", path);
			return fd;
		}

		if (fallback_fd < 0) {
			fallback_fd = fd;
			snprintf(fallback_path, sizeof(fallback_path), "%s", path);
			continue;
		}

		close(fd);
	}

	if (fallback_fd >= 0) {
		snprintf(chip_path, chip_path_size, "%s", fallback_path);
		return fallback_fd;
	}

	return -ENOENT;
}

static int linux_gpio_request_line(struct no_os_gpio_desc *desc, uint32_t flags,
				   uint8_t default_value)
{
	struct linux_gpio_desc *linux_desc = desc->extra;
	struct gpiohandle_request req = {0};
	int chip_fd;

	if (linux_desc->line_fd >= 0) {
		close(linux_desc->line_fd);
		linux_desc->line_fd = -1;
	}

	chip_fd = linux_gpio_open_chip_for_offset(linux_desc->offset,
						  linux_desc->chip_path,
						  sizeof(linux_desc->chip_path));
	if (chip_fd < 0)
		return chip_fd;

	req.lines = 1;
	req.lineoffsets[0] = linux_desc->offset;
	req.flags = flags;
	req.default_values[0] = default_value ? 1 : 0;
	snprintf(req.consumer_label, sizeof(req.consumer_label), "init_ad9361");

	if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
		int ret = -errno;
		close(chip_fd);
		return ret;
	}

	close(chip_fd);

	linux_desc->line_fd = req.fd;
	linux_desc->direction = (flags & GPIOHANDLE_REQUEST_OUTPUT) ?
				NO_OS_GPIO_OUT : NO_OS_GPIO_IN;
	linux_desc->has_direction = 1;

	return 0;
}

/**
 * @brief Obtain the GPIO decriptor.
 * @param desc - The GPIO descriptor.
 * @param param - GPIO initialization parameters
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_get(struct no_os_gpio_desc **desc,
		       const struct no_os_gpio_init_param *param)
{
	struct linux_gpio_desc *linux_desc;
	struct no_os_gpio_desc *descriptor;

	descriptor = (struct no_os_gpio_desc *)malloc(sizeof(*descriptor));
	if (!descriptor)
		return -1;

	linux_desc = (struct linux_gpio_desc *)malloc(sizeof(*linux_desc));
	if (!linux_desc)
		goto free_desc;

	memset(linux_desc, 0, sizeof(*linux_desc));
	linux_desc->line_fd = -1;
	linux_desc->offset = param->number;

	descriptor->extra = linux_desc;
	descriptor->number = param->number;

	*desc = descriptor;

	return 0;

free_linux_desc:
	free(linux_desc);
free_desc:
	free(descriptor);

	return -1;
}

/**
 * @brief Get the value of an optional GPIO.
 * @param desc - The GPIO descriptor.
 * @param param - GPIO Initialization parameters.
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_get_optional(struct no_os_gpio_desc **desc,
				const struct no_os_gpio_init_param *param)
{
	if (!param || param->number == -1) {
		*desc = NULL;
		return 0;
	}

	return linux_gpio_get(desc, param);
}

/**
 * @brief Free the resources allocated by no_os_gpio_get().
 * @param desc - The SPI descriptor.
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_remove(struct no_os_gpio_desc *desc)
{
	struct linux_gpio_desc *linux_desc;

	linux_desc = desc->extra;

	if (linux_desc->line_fd >= 0)
		close(linux_desc->line_fd);

	free(desc->extra);
	free(desc);

	return 0;
}

/**
 * @brief Set the value of the specified GPIO.
 * @param desc - The GPIO descriptor.
 * @param value - The value.
 *                Example: NO_OS_GPIO_HIGH
 *                         NO_OS_GPIO_LOW
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_set_value(struct no_os_gpio_desc *desc,
			     uint8_t value)
{
	struct linux_gpio_desc *linux_desc;
	struct gpiohandle_data data = {0};
	int ret;

	linux_desc = desc->extra;

	if (linux_desc->line_fd < 0 ||
	    linux_desc->direction != NO_OS_GPIO_OUT) {
		ret = linux_gpio_request_line(desc, GPIOHANDLE_REQUEST_OUTPUT,
					      value);
		if (ret < 0) {
			printf("%s: Can't request GPIO output line %d: %s\n\r",
			       __func__, desc->number, strerror(-ret));
			return -1;
		}
	}

	data.values[0] = value ? 1 : 0;
	if (ioctl(linux_desc->line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL,
		  &data) < 0) {
		printf("%s: Can't set GPIO line %d: %s\n\r",
		       __func__, desc->number, strerror(errno));
		return -1;
	}

	return 0;
}

/**
 * @brief Get the value of the specified GPIO.
 * @param desc - The GPIO descriptor.
 * @param value - The value.
 *                Example: NO_OS_GPIO_HIGH
 *                         NO_OS_GPIO_LOW
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_get_value(struct no_os_gpio_desc *desc,
			     uint8_t *value)
{
	struct linux_gpio_desc *linux_desc;
	struct gpiohandle_data data = {0};
	int ret;

	linux_desc = desc->extra;

	if (linux_desc->line_fd < 0) {
		ret = linux_gpio_request_line(desc, GPIOHANDLE_REQUEST_INPUT, 0);
		if (ret < 0) {
			printf("%s: Can't request GPIO input line %d: %s\n\r",
			       __func__, desc->number, strerror(-ret));
			return -1;
		}
	}

	if (ioctl(linux_desc->line_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL,
		  &data) < 0) {
		printf("%s: Can't get GPIO line %d: %s\n\r",
		       __func__, desc->number, strerror(errno));
		return -1;
	}

	if (data.values[0] == 0)
		*value = NO_OS_GPIO_LOW;
	else
		*value = NO_OS_GPIO_HIGH;

	return 0;
}

/**
 * @brief Enable the input direction of the specified GPIO.
 * @param desc - The GPIO descriptor.
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_direction_input(struct no_os_gpio_desc *desc)
{
	int ret;

	ret = linux_gpio_request_line(desc, GPIOHANDLE_REQUEST_INPUT, 0);
	if (ret < 0) {
		printf("%s: Can't request GPIO input line %d: %s\n\r",
		       __func__, desc->number, strerror(-ret));
		return -1;
	}

	return 0;
}

/**
 * @brief Enable the output direction of the specified GPIO.
 * @param desc - The GPIO descriptor.
 * @param value - The value.
 *                Example: NO_OS_GPIO_HIGH
 *                         NO_OS_GPIO_LOW
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_direction_output(struct no_os_gpio_desc *desc,
				    uint8_t value)
{
	int ret;

	ret = linux_gpio_request_line(desc, GPIOHANDLE_REQUEST_OUTPUT, value);
	if (ret < 0) {
		printf("%s: Can't request GPIO output line %d: %s\n\r",
		       __func__, desc->number, strerror(-ret));
		return -1;
	}

	return 0;
}

/**
 * @brief Get the direction of the specified GPIO.
 * @param desc - The GPIO descriptor.
 * @param direction - The direction.
 *                    Example: NO_OS_GPIO_OUT
 *                             NO_OS_GPIO_IN
 * @return 0 in case of success, -1 otherwise.
 */
int32_t linux_gpio_get_direction(struct no_os_gpio_desc *desc,
				 uint8_t *direction)
{
	struct linux_gpio_desc *linux_desc;

	linux_desc = desc->extra;

	if (!linux_desc->has_direction) {
		printf("%s: GPIO line %d direction was not requested\n\r",
		       __func__, desc->number);
		return -1;
	}

	*direction = linux_desc->direction;

	return 0;
}

/**
 * @brief Linux platform specific GPIO platform ops structure
 */
const struct no_os_gpio_platform_ops linux_gpio_ops = {
	.gpio_ops_get = &linux_gpio_get,
	.gpio_ops_get_optional = &linux_gpio_get_optional,
	.gpio_ops_remove = &linux_gpio_remove,
	.gpio_ops_direction_input = &linux_gpio_direction_input,
	.gpio_ops_direction_output = &linux_gpio_direction_output,
	.gpio_ops_get_direction = &linux_gpio_get_direction,
	.gpio_ops_set_value = &linux_gpio_set_value,
	.gpio_ops_get_value = &linux_gpio_get_value,
};
