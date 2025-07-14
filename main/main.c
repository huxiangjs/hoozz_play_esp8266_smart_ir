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
 */

#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_spi_flash.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "event_bus.h"
#include "keyboard.h"
#include "spiffs.h"
#include "simple_ctrl.h"
#include "wifi.h"
#include "ir_ctrl.h"

static const char *TAG = "APP-MAIN";

#define KEYBOARD_GPIO_PIN		GPIO_NUM_0
#define IR_RX_GPIO_PIN			GPIO_NUM_12
#define IR_TX_GPIO_PIN			GPIO_NUM_14

static bool config_mode;

static void app_show_info(void)
{
	int size = esp_get_free_heap_size();

	ESP_LOGI(TAG, "Free heap size: %dbytes", size);
}

static void print_chip_info(void)
{
	/* Print chip information */
	esp_chip_info_t chip_info;

	esp_chip_info(&chip_info);
	ESP_LOGI(TAG, "This is ESP8266 chip with %d CPU cores, WiFi, silicon revision %d, %dMB %s flash",
		 chip_info.cores, chip_info.revision, spi_flash_get_chip_size() / (1024 * 1024),
		 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

static int app_ir_request(char *buffer, int buf_offs, int vaild_size, int buff_size)
{

	ESP_LOGI(TAG, "enter.");

	return 0;
}

static bool app_event_notify_callback(struct event_bus_msg *msg)
{
	switch (msg->type) {
	case EVENT_BUS_STARTUP:
		break;
	case EVENT_BUS_START_SMART_CONFIG:
		break;
	case EVENT_BUS_STOP_SMART_CONFIG:
		config_mode = false;
		break;
	case EVENT_BUS_KEYBOARD:
		if (config_mode || msg->param1 != KEYBOARD_GPIO_PIN)
			break;
		if ((msg->param2 == KEYBOARD_EVENT_SHORT_RELEASE)) {
			ESP_LOGW(TAG, "Restarting now");
			fflush(stdout);
			esp_restart();
		}
		/* Smart config */
		if (msg->param2 == KEYBOARD_EVENT_LONG_RELEASE) {
			ESP_LOGI(TAG, "Smart config");
			wifi_smartconfig();
			config_mode = true;
		}
		break;
	case EVENT_BUS_IR_RX:
		ESP_LOGI(TAG, "IR received");
		break;
	}

	return false;
}

static void app_ir_rx_callback(uint16_t *data, uint16_t len)
{
	uint16_t index;

	ESP_LOGI(TAG, "IR data len: %u", len);

	for (index = 0; index < len; index++)
		printf("%u ", data[index]);
	printf("\n");
	fflush(stdout);

	ir_ctrl_tx_send_data(data, len);
}

void app_main(void)
{
	struct event_bus_msg msg = { EVENT_BUS_STARTUP, 0, 0};
	uint8_t keyboard_gpios[] = { KEYBOARD_GPIO_PIN };

	print_chip_info();
	app_show_info();
	ESP_ERROR_CHECK(nvs_flash_init());

	/* install gpio isr service */
	ESP_ERROR_CHECK(gpio_install_isr_service(0));

	spiffs_init();

	/* Event bus */
	event_bus_init();
	event_bus_register(app_event_notify_callback);
	event_bus_send(&msg);

	/* Keyboard */
	keyboard_init(keyboard_gpios, sizeof(keyboard_gpios));

	/* IR ctrl */
	ir_ctrl_init(IR_RX_GPIO_PIN, IR_TX_GPIO_PIN, app_ir_rx_callback);

	/* Wifi */
	wifi_init();

	/* Network ctrl */
	simple_ctrl_init();
	simple_ctrl_set_name("SMART IR");
	simple_ctrl_set_class_id(CLASS_ID_SMART_IR);
	simple_ctrl_request_register(app_ir_request);
	wifi_connect();

	app_show_info();
}
