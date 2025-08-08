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
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
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

#define SPIFFS_ROOT_PATH		"/spiffs"
#define SPIFFS_ROOT_PATH_LEN		(sizeof(SPIFFS_ROOT_PATH) - 1)
#define IR_DATA_PREFIX			"IRDATA-"
#define IR_DATA_PREFIX_LEN		(sizeof(IR_DATA_PREFIX) - 1)

#define IR_CMD_GET_COUNT		0x00
#define IR_CMD_GET_ITEM			0x01
#define IR_CMD_TX_TEST			0x02
#define IR_CMD_SAVE			0x03
#define IR_CMD_REMOVE			0x04
#define IR_CMD_TX_SEND			0x05

#define IR_NOTIFY_TYPE_RX		0x00
#define IR_NOTIFY_TYPE_KEY		0x01
#define IR_NOTIFY_TYPE_TX		0x02

#define IR_RESULT_OK			0x00
#define IR_RESULT_FAIL			0x01
#define IR_RESULT_DONE			0x02

#define IR_KEY_NUM_MAX			100
#define IR_KEY_NAME_LEN_MAX		100

static bool config_mode;
static QueueHandle_t led_queue;
static SemaphoreHandle_t ir_mutex;
static uint16_t ir_rx_buff[RX_BUFF_SIZE_MAX];
static uint16_t ir_rx_len;
static uint32_t ir_rx_count;

struct key_info {
	bool vaild;
	char name[IR_KEY_NAME_LEN_MAX + 1];
};

static uint32_t ir_key_count;
static struct key_info ir_key_info[IR_KEY_NUM_MAX];

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

/* Rebuild cache when key is added/removed */
static void app_ir_key_cache_build(void)
{
	DIR *dp;
	struct dirent *ep;
	uint32_t file_count = 0;

	dp = opendir(SPIFFS_ROOT_PATH);
	ESP_ERROR_CHECK(dp == NULL);
	// rewinddir(dp);

	ir_key_count = 0;
	memset(ir_key_info, 0, sizeof(ir_key_info));

	printf("File list:\n");
	while ((ep = readdir(dp))) {
		printf("    %s/%s\n", SPIFFS_ROOT_PATH, ep->d_name);
		if (!strncmp(ep->d_name, IR_DATA_PREFIX, IR_DATA_PREFIX_LEN)) {
			strncpy(ir_key_info[ir_key_count].name, ep->d_name + IR_DATA_PREFIX_LEN,
				sizeof(ir_key_info[ir_key_count].name));
			ir_key_info[ir_key_count].vaild = true;
			ir_key_count++;
		}
		file_count++;
	}
	printf("File count: %u, Key count: %u\n", file_count, ir_key_count);

	closedir(dp);
}

/* When a key is added/removed, notify all connected devices to update the list */
static void app_notify_key_changed(void)
{
	char buffer[5];

	buffer[0] = IR_NOTIFY_TYPE_KEY;
	buffer[1] = (char)((ir_key_count >> 0) & 0xff);
	buffer[2] = (char)((ir_key_count >> 8) & 0xff);
	buffer[3] = (char)((ir_key_count >> 16) & 0xff);
	buffer[4] = (char)((ir_key_count >> 24) & 0xff);
	simple_ctrl_notify(buffer, sizeof(buffer));
}

static void app_led_set_step(char step)
{
	xQueueSend(led_queue, (void *)&step, (TickType_t)0);
}

static int app_ir_request(char *buffer, int buf_offs, int vaild_size, int buff_size)
{
	uint32_t count;
	size_t len;
	FILE *fp;
	char file_path[SPIFFS_ROOT_PATH_LEN + 1 + IR_DATA_PREFIX_LEN + IR_KEY_NAME_LEN_MAX + 1];

	if (vaild_size < 1) {
		ESP_LOGE(TAG, "Information command is incorrect");
		return -1;
	}

	if (buff_size - buf_offs < 2)
		goto nomem_err;

	switch (buffer[buf_offs]) {
	case IR_CMD_GET_COUNT:		/* Get the number of keys */
		if (buff_size - buf_offs < 6)
			goto nomem_err;
		buffer[buf_offs + 1] = IR_RESULT_OK;
		buffer[buf_offs + 2] = (ir_key_count >> 0) & 0xff;
		buffer[buf_offs + 3] = (ir_key_count >> 8) & 0xff;
		buffer[buf_offs + 4] = (ir_key_count >> 16) & 0xff;
		buffer[buf_offs + 5] = (ir_key_count >> 24) & 0xff;
		return 6;
	case IR_CMD_GET_ITEM:		/* Get key information */
		if (vaild_size != 5)
			goto lllegal_err;
		if (buff_size - buf_offs < 2 + IR_KEY_NAME_LEN_MAX)
			goto nomem_err;
		count = (buffer[buf_offs + 1] << 0) |
			(buffer[buf_offs + 2] << 8) |
			(buffer[buf_offs + 3] << 16) |
			(buffer[buf_offs + 4] << 24);
		len = 0;
		if (count >= IR_KEY_NUM_MAX) {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "Get failed, count incorrect");
		} else {
			if (ir_key_info[count].vaild) {
				len = strlen(ir_key_info[count].name);
				memcpy(buffer + buf_offs + 2, ir_key_info[count].name, len);
				buffer[buf_offs + 1] = IR_RESULT_OK;
			} else {
				buffer[buf_offs + 1] = IR_RESULT_DONE;
				ESP_LOGW(TAG, "Count invalid");
			}
		}
		return 2 + len;
	case IR_CMD_TX_TEST:		/* Testing received IR data */
		if (vaild_size != 5)
			goto lllegal_err;
		app_led_set_step('F');
		count = (buffer[buf_offs + 1] << 0) |
			(buffer[buf_offs + 2] << 8) |
			(buffer[buf_offs + 3] << 16) |
			(buffer[buf_offs + 4] << 24);
		xSemaphoreTake(ir_mutex, portMAX_DELAY);
		if (count == ir_rx_count) {
			ir_ctrl_tx_send_data(ir_rx_buff, ir_rx_len);
			buffer[buf_offs + 1] = IR_RESULT_OK;
			// ESP_LOGI(TAG, "Test okay");
		} else {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "Test failed, count incorrect");
		}
		xSemaphoreGive(ir_mutex);
		return 2;
	case IR_CMD_SAVE:		/* Save the received IR key data */
		if (vaild_size < 1 + 4)
			goto lllegal_err;
		if (vaild_size - 1 - 4 > IR_KEY_NAME_LEN_MAX) {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "Save failed (The name is too long)");
			return 2;
		}
		count = (buffer[buf_offs + 1] << 0) |
			(buffer[buf_offs + 2] << 8) |
			(buffer[buf_offs + 3] << 16) |
			(buffer[buf_offs + 4] << 24);
		xSemaphoreTake(ir_mutex, portMAX_DELAY);
		if (count == ir_rx_count) {
			snprintf(file_path, sizeof(file_path), SPIFFS_ROOT_PATH "/" IR_DATA_PREFIX "%.*s",
				 vaild_size - 1 - 4, buffer + buf_offs + 1 + 4);
			ESP_LOGI(TAG, "Save file path: %s", file_path);
			fp = fopen(file_path, "wb+");
			if (fp) {
				len = fwrite(ir_rx_buff, sizeof(uint16_t), ir_rx_len, fp);
				fclose(fp);
				if (len == ir_rx_len) {
					/* Re-build cache */
					app_ir_key_cache_build();
					/* Notify all connections that the key has changed */
					app_notify_key_changed();
					buffer[buf_offs + 1] = IR_RESULT_OK;
					// ESP_LOGI(TAG, "Save okay");
				} else {
					remove(file_path);
					buffer[buf_offs + 1] = IR_RESULT_FAIL;
					ESP_LOGE(TAG, "Save failed (Write)");
				}
			} else {
				buffer[buf_offs + 1] = IR_RESULT_FAIL;
				ESP_LOGE(TAG, "Save failed (Open)");
			}
		} else {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "Save failed (Count incorrect)");
		}
		xSemaphoreGive(ir_mutex);
		return 2;
	case IR_CMD_REMOVE:		/* Delete the specified key */
		if (vaild_size - 1 > IR_KEY_NAME_LEN_MAX) {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "Remove failed (The name is too long)");
			return 2;
		}
		snprintf(file_path, sizeof(file_path), SPIFFS_ROOT_PATH "/" IR_DATA_PREFIX "%.*s",
			 vaild_size - 1, buffer + buf_offs + 1);
		ESP_LOGI(TAG, "Remove file path: %s", file_path);
		if (remove(file_path)) {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "Remove failed");
		} else {
			/* Re-build cache */
			app_ir_key_cache_build();
			/* Notify all connections that the key has changed */
			app_notify_key_changed();
			buffer[buf_offs + 1] = IR_RESULT_OK;
			// ESP_LOGI(TAG, "Remove okay");
		}
		return 2;
	case IR_CMD_TX_SEND:		/* Send key signal */
		if (vaild_size - 1 > IR_KEY_NAME_LEN_MAX) {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "IR Send failed (The name is too long)");
			return 2;
		}
		snprintf(file_path, sizeof(file_path), SPIFFS_ROOT_PATH "/" IR_DATA_PREFIX "%.*s",
			 vaild_size - 1, buffer + buf_offs + 1);
		ESP_LOGI(TAG, "IR data file path: %s", file_path);
		fp = fopen(file_path, "rb");
		if (fp) {
			len = fread(ir_rx_buff, sizeof(uint16_t), RX_BUFF_SIZE_MAX, fp);
			fclose(fp);
			ESP_LOGI(TAG, "IR data %dp", len);
			if (len > 0) {
				app_led_set_step('F');
				ir_rx_len = (uint16_t)len;
				ir_ctrl_tx_send_data(ir_rx_buff, ir_rx_len);
				/* Notify send done */
				buffer[buf_offs + 0] = IR_NOTIFY_TYPE_TX;
				simple_ctrl_notify(buffer + buf_offs, vaild_size);
				/* Return OK */
				buffer[buf_offs + 0] = IR_CMD_TX_SEND;
				buffer[buf_offs + 1] = IR_RESULT_OK;
				// ESP_LOGI(TAG, "IR send okay");
			} else {
				buffer[buf_offs + 1] = IR_RESULT_FAIL;
				ESP_LOGE(TAG, "IR send failed (Read)");
			}
		} else {
			buffer[buf_offs + 1] = IR_RESULT_FAIL;
			ESP_LOGE(TAG, "IR send failed (Open)");
		}
		return 2;
	default:
		goto lllegal_err;
	}

	return 0;
nomem_err:
	ESP_LOGE(TAG, "Not enough buffer space");
	return -1;
lllegal_err:
	ESP_LOGE(TAG, "Lllegal command");
	return -1;
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
	char buffer[7];

	ESP_LOGI(TAG, "IR data len: %up, rx count: %u", len, ir_rx_count);
	app_led_set_step('F');

	xSemaphoreTake(ir_mutex, portMAX_DELAY);
	memcpy(ir_rx_buff, data, len * sizeof(uint16_t));
	ir_rx_len = len;
	ir_rx_count++;
	xSemaphoreGive(ir_mutex);

	/* Notify all connected devices that IR key data has been received */
	buffer[0] = IR_NOTIFY_TYPE_RX;
	buffer[1] = (char)((ir_rx_count >> 0) & 0xff);
	buffer[2] = (char)((ir_rx_count >> 8) & 0xff);
	buffer[3] = (char)((ir_rx_count >> 16) & 0xff);
	buffer[4] = (char)((ir_rx_count >> 24) & 0xff);
	buffer[5] = (char)((len >> 0) & 0xff);
	buffer[6] = (char)((len >> 8) & 0xff);
	simple_ctrl_notify(buffer, sizeof(buffer));
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

	/* IR */
	ir_mutex = xSemaphoreCreateMutex();
	app_ir_key_cache_build();
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
