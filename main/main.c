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
#include <freertos/queue.h>
#include <freertos/semphr.h>
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
#include "gpio_led.h"

static const char *TAG = "APP-MAIN";

#define KEYBOARD_GPIO_PIN		GPIO_NUM_0
#define IR_RX_GPIO_PIN			GPIO_NUM_12
#define IR_TX_GPIO_PIN			GPIO_NUM_14
#define LED_GREEN_GPIO_PIN		GPIO_NUM_4
#define LED_RED_GPIO_PIN		GPIO_NUM_5

#define LED_GREEN_BRIGHTNESS_MAX	32
#define LED_RED_BRIGHTNESS_MAX		255

static bool config_mode;
static QueueHandle_t led_queue;

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

static void app_led_task(void *arg)
{
	TickType_t xTicksToWait = portMAX_DELAY;
	char step;
	uint8_t red_value = 0;
	uint8_t green_value = 0;
	bool is_green = false;
	char recyclable_step = 0;

	while(1) {
		xQueueReceive(led_queue, &step, xTicksToWait);
		switch (step) {
		case 'r':
			xTicksToWait = portMAX_DELAY;
			green_value = 0;
			red_value = LED_RED_BRIGHTNESS_MAX;
			is_green = false;
			recyclable_step = step;
			break;
		case 'g':
			xTicksToWait = pdMS_TO_TICKS(10000);;
			red_value = 0;
			green_value = LED_GREEN_BRIGHTNESS_MAX;
			is_green = true;
			recyclable_step = step;
			step = 'P';
			break;
		case 'C':
			xTicksToWait = pdMS_TO_TICKS(100);
			red_value = red_value ? 0 : LED_RED_BRIGHTNESS_MAX;
			green_value = red_value ? 0 : LED_GREEN_BRIGHTNESS_MAX;
			recyclable_step = step;
			break;
		case 'P':
			xTicksToWait = portMAX_DELAY;
			green_value = 0;
			recyclable_step = step;
			break;
		case (char)-'P':
			recyclable_step = step;
			if (is_green) {
				xTicksToWait = 0;
				step = 'g';
			} else {
				xTicksToWait = portMAX_DELAY;
			}
			break;
		case 'F':
			gpio_led_set_red_brightness(LED_RED_BRIGHTNESS_MAX);
			gpio_led_set_green_brightness(0);
			vTaskDelay(pdMS_TO_TICKS(40));
			gpio_led_set_red_brightness(0);
			gpio_led_set_green_brightness(LED_GREEN_BRIGHTNESS_MAX);
			vTaskDelay(pdMS_TO_TICKS(40));
			step = recyclable_step;
			break;
		default:
			ESP_LOGW(TAG, "unknow step: %d", step);
		}
		gpio_led_set_red_brightness(red_value);
		gpio_led_set_green_brightness(green_value);
	}

	vQueueDelete(led_queue);
	vTaskDelete(NULL);
}

static inline void app_led_set_step(char step)
{
	xQueueSend(led_queue, (void *)&step, (TickType_t)0);
}

static bool app_event_notify_callback(struct event_bus_msg *msg)
{
	switch (msg->type) {
	case EVENT_BUS_STARTUP:
		app_led_set_step('r');
		break;
	case EVENT_BUS_WIFI_CONNECTED:
		app_led_set_step('g');
		break;
	case EVENT_BUS_WIFI_DISCONNECTED:
		app_led_set_step('r');
		break;
	case EVENT_BUS_START_SMART_CONFIG:
		app_led_set_step('C');
		break;
	case EVENT_BUS_STOP_SMART_CONFIG:
		config_mode = false;
		break;
	case EVENT_BUS_KEYBOARD:
		if (config_mode || msg->param1 != KEYBOARD_GPIO_PIN)
			break;
		if ((msg->param2 == KEYBOARD_EVENT_SHORT_RELEASE)) {
			// ESP_LOGW(TAG, "Restarting now");
			// fflush(stdout);
			// esp_restart();
			app_led_set_step(-'P');
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

	app_led_set_step('F');
	ir_ctrl_tx_send_data(data, len);
}

void app_main(void)
{
	struct event_bus_msg msg = { EVENT_BUS_STARTUP, 0, 0};
	uint8_t keyboard_gpios[] = { KEYBOARD_GPIO_PIN };

	print_chip_info();
	app_show_info();

	/* GPIO LED */
	gpio_led_init(LED_RED_GPIO_PIN, LED_GREEN_GPIO_PIN);
	led_queue = xQueueCreate(10, sizeof(char));
	ESP_ERROR_CHECK(led_queue == NULL);
	ESP_ERROR_CHECK(xTaskCreate(app_led_task, "app_led_task", 1024,
			NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS);

	/* NVS init */
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
