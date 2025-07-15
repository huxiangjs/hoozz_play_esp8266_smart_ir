/*
 * MIT License
 *
 * Copyright (c) 2025 huxiangjs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * refs: ESP8266_RTOS_SDK/examples/peripherals/ledc/main/ledc_example_main.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/ledc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <esp8266/gpio_struct.h>

#define LEDC_SPEED_MODE			LEDC_HIGH_SPEED_MODE
#define LEDC_CHANNEL_RED		LEDC_CHANNEL_0
#define LEDC_CHANNEL_GREEN		LEDC_CHANNEL_1
#define LEDC_MAX_DUTY			8196
#define ARRAY_SIZE(a)			(sizeof(a) / sizeof(a[0]))

#define FUNC_GPIO_INVALID		0x7
#define PERIPHS_GPIO_FUNC(i)		\
	(i == 0)	? FUNC_GPIO0	: \
	(i == 1)	? FUNC_GPIO1	: \
	(i == 2)	? FUNC_GPIO2	: \
	(i == 3)	? FUNC_GPIO3	: \
	(i == 4)	? FUNC_GPIO4	: \
	(i == 5)	? FUNC_GPIO5	: \
	(i == 6)	? FUNC_GPIO6	: \
	(i == 7)	? FUNC_GPIO7	: \
	(i == 8)	? FUNC_GPIO8	: \
	(i == 9)	? FUNC_GPIO9	: \
	(i == 10)	? FUNC_GPIO10	: \
	(i == 11)	? FUNC_GPIO11	: \
	(i == 12)	? FUNC_GPIO12	: \
	(i == 13)	? FUNC_GPIO13	: \
	(i == 14)	? FUNC_GPIO14	: \
	(i == 15)	? FUNC_GPIO15	: \
	FUNC_GPIO_INVALID

static inline uint16_t gpio_led_brightness_to_duty(uint8_t value)
{
	return LEDC_MAX_DUTY - (value * LEDC_MAX_DUTY / 0xff);
}

static inline void gpio_led_set_brightness(uint8_t channel, uint8_t value)
{
	ledc_set_duty(LEDC_SPEED_MODE, channel, gpio_led_brightness_to_duty(value));
	ledc_update_duty(LEDC_SPEED_MODE, channel);
}

void gpio_led_set_red_brightness(uint8_t value)
{
	gpio_led_set_brightness(LEDC_CHANNEL_RED, value);
}

void gpio_led_set_green_brightness(uint8_t value)
{
	gpio_led_set_brightness(LEDC_CHANNEL_GREEN, value);
}

#if 0
static inline void gpio_led_set_fade_no_wait(uint8_t channel, uint8_t value, uint16_t ms)
{
	ledc_set_fade_with_time(LEDC_SPEED_MODE, channel, gpio_led_brightness_to_duty(value), ms);
	ledc_fade_start(LEDC_SPEED_MODE, channel, LEDC_FADE_NO_WAIT);
}

void gpio_led_set_red_fade(uint8_t value, uint16_t ms)
{
	gpio_led_set_fade_no_wait(LEDC_CHANNEL_RED, value, ms);
}

void gpio_led_set_green_fade(uint8_t value, uint16_t ms)
{
	gpio_led_set_fade_no_wait(LEDC_CHANNEL_GREEN, value, ms);
}
#endif

void gpio_led_init(uint8_t red_gpio_num, uint8_t green_gpio_num)
{
	int ch;

	ledc_timer_config_t ledc_timer = {
		.duty_resolution = LEDC_TIMER_13_BIT,	// resolution of PWM duty
		.freq_hz = 1000,			// frequency of PWM signal
		.speed_mode = LEDC_SPEED_MODE,		// timer mode
		.timer_num = LEDC_TIMER_0		// timer index
	};

	ledc_channel_config_t ledc_channel[] = {
		[LEDC_CHANNEL_RED] = {
			.channel    = LEDC_CHANNEL_RED,
			.duty       = 0,
			.gpio_num   = red_gpio_num,
			.speed_mode = LEDC_SPEED_MODE,
			.hpoint     = 0,
			.timer_sel  = LEDC_TIMER_0
		},
		[LEDC_CHANNEL_GREEN] = {
			.channel    = LEDC_CHANNEL_GREEN,
			.duty       = 0,
			.gpio_num   = green_gpio_num,
			.speed_mode = LEDC_SPEED_MODE,
			.hpoint     = 0,
			.timer_sel  = LEDC_TIMER_0
		}
	};

	/*
	 * This is a workaround. Because there is a problem with the parameter check of
	 * the `ledc_channel_config()` function in the SDK, the duty cannot be configured
	 * to the maximum. Therefore, we first set the mux to invalid, and then switch
	 * it back after the duty is configured.
	 */
	PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(red_gpio_num), FUNC_GPIO_INVALID);
	PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(green_gpio_num), FUNC_GPIO_INVALID);

	/* set timer */
	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

	/* set LED Controller with previously prepared configuration */
	for (ch = 0; ch < ARRAY_SIZE(ledc_channel); ch++)
		ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel[ch]));

	/* initialize fade service */
	ESP_ERROR_CHECK(ledc_fade_func_install(0));

	/* workaround: set duty */
	for (ch = 0; ch < ARRAY_SIZE(ledc_channel); ch++) {
		ledc_stop(ledc_channel[ch].speed_mode, ledc_channel[ch].channel, 0);
		gpio_led_set_brightness(ledc_channel[ch].channel, 0);
	}
	portYIELD();

	/* workaround: mux switch to GPIO */
	PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(red_gpio_num), PERIPHS_GPIO_FUNC(red_gpio_num));
	PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(green_gpio_num), PERIPHS_GPIO_FUNC(green_gpio_num));
}
