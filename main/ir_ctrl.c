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
 * refs: ESP8266_RTOS_SDK/components/esp8266/driver/ir_tx.c
 *       ESP8266_RTOS_SDK/examples/peripherals/ir_tx/main/ir_tx_example_main.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <driver/hw_timer.h>
#include <driver/i2s.h>
#include <driver/ir_tx.h>
#include <esp8266/gpio_struct.h>

#include "ir_ctrl.h"
#include "event_bus.h"

static const char *TAG = "IR-CTRL";

#define ARRAY_SIZE(a)			(sizeof(a) / sizeof(a[0]))
#define GPIO_INPUT_PIN_SEL(NUM)		(1ULL << (NUM))
#define IR_TX_IDLE_LEVEL		0

#if defined(CONFIG_IDF_TARGET_ESP8266)
#define portYIELD_FROM_ISR portYIELD
#endif

static QueueHandle_t detail_queue;
static uint8_t ir_rx_gpio;
static uint16_t ir_rx_buff[RX_BUFF_SIZE_MAX];
static uint16_t ir_rx_len = 0;
static ir_rx_callback ir_rx_cb;

static TaskHandle_t waiting_task;
static uint8_t ir_tx_gpio;
static uint16_t *ir_tx_buff;
static uint16_t ir_tx_size;
static uint16_t ir_tx_count;
static uint8_t ir_tx_step;

/* No pulse output */
static void inline ir_tx_clear_carrier()
{
	if (ir_tx_gpio == GPIO_NUM_2)
		// GPIO.out_w1tc |= 0x4;	// GPIO 2
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
	else if (ir_tx_gpio == GPIO_NUM_14)
		// GPIO.out_w1tc |= 0x4000;	// GPIO 14
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14);
}

/* Output 38kHz pulse */
static void inline ir_tx_gen_carrier()
{
	if (ir_tx_gpio == GPIO_NUM_2)
		// GPIO.out_w1ts |= 0x4;	// GPIO 2
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_I2SO_WS);
	else if (ir_tx_gpio == GPIO_NUM_14)
		// GPIO.out_w1ts |= 0x4000;	// GPIO 14
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_I2SI_WS);
}

static void inline ir_tx_timer_alarm(uint32_t val)
{
	hw_timer_alarm_us(val - IR_TX_HW_TIMER_ERROR_US, false);
}

static void IRAM_ATTR ir_tx_handler(void *arg)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	/* done? */
	if (ir_tx_count == ir_tx_size) {
		ir_tx_clear_carrier();
		vTaskNotifyGiveFromISR(waiting_task, &xHigherPriorityTaskWoken);
		if (xHigherPriorityTaskWoken != pdFALSE)
			portYIELD_FROM_ISR();
		return;
	}

	/* set next alarm */
	ir_tx_timer_alarm(ir_tx_buff[ir_tx_count]);
	ir_tx_step = !ir_tx_step;
	if (ir_tx_step)
		ir_tx_gen_carrier();
	else
		ir_tx_clear_carrier();

	ir_tx_count++;
}

static void ir_rx_enable(void)
{
	gpio_set_intr_type(ir_rx_gpio, GPIO_INTR_ANYEDGE);
}

static void ir_rx_disable(void)
{
	gpio_set_intr_type(ir_rx_gpio, GPIO_INTR_DISABLE);
}

void ir_ctrl_tx_send_data(uint16_t *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		ESP_LOGW(TAG, "TX buffer is NULL or length is 0");
		return;
	}

	waiting_task = xTaskGetCurrentTaskHandle();
	ir_tx_buff = data;
	ir_tx_size = len;
	ir_tx_count = 0;
	ir_tx_step = 0;

	ir_rx_disable();
	/* start tx */
	ir_tx_handler(NULL);
	/* wait until completion */
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	ir_rx_enable();
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	static int64_t last_us;
	int64_t cur_us = esp_timer_get_time();
	uint16_t detail = (uint16_t)(cur_us - last_us);

	xQueueSendFromISR(detail_queue, &detail, &xHigherPriorityTaskWoken);
	if (xHigherPriorityTaskWoken != pdFALSE)
		portYIELD_FROM_ISR();

	last_us = cur_us;
}

static void ir_rx_task(void *arg)
{
	TickType_t xTicksToWait = portMAX_DELAY;
	struct event_bus_msg msg = { EVENT_BUS_IR_RX, 0, 0};
	uint16_t detail;
	bool drop = false;

	while (1) {
		if (xQueueReceive(detail_queue, &detail, xTicksToWait)) {
			if (xTicksToWait == portMAX_DELAY) {	// start
				xTicksToWait = pdMS_TO_TICKS(60);
				ir_rx_len = 0;
				drop = false;
			} else if (ir_rx_len < ARRAY_SIZE(ir_rx_buff)) { // append
				ir_rx_buff[ir_rx_len++] = detail;
				if (detail < 300)
					drop = true;
			}
		} else {					// stop
			xTicksToWait = portMAX_DELAY;
			if (drop || ir_rx_len < 32)		// filter out interference signals
				continue;
			ESP_LOGD(TAG, "wave length: %u\n", ir_rx_len);
			// ir_ctrl_tx_send_data(ir_rx_buff, ir_rx_len);
			if (ir_rx_cb)
				ir_rx_cb(ir_rx_buff, ir_rx_len);
			event_bus_send(&msg);
		}
	}
}

static void ir_gpio_init(void)
{
	gpio_config_t io_conf;

	/* RX GPIO */
	memset(&io_conf, 0, sizeof(io_conf));
	io_conf.intr_type = GPIO_INTR_ANYEDGE;
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL(ir_rx_gpio);
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&io_conf));
	/* hook isr handler for specific gpio pin */
	ESP_ERROR_CHECK(gpio_isr_handler_add(ir_rx_gpio, gpio_isr_handler, NULL));

	/* TX GPIO */
	memset(&io_conf, 0, sizeof(io_conf));
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL(ir_tx_gpio);
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
	ESP_ERROR_CHECK(gpio_config(&io_conf));
	ESP_ERROR_CHECK(gpio_set_level(ir_tx_gpio, IR_TX_IDLE_LEVEL));
}

static void ir_i2s_init(void)
{
	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER,	// only carrier mode
		.sample_rate = 38000,		// 38kHz
		.bits_per_sample = 16,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,	// 2-channels
		.communication_format = I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
		.dma_buf_count = 2,		// no use
		.dma_buf_len = 8		// no use
	};

	i2s_pin_config_t pin_config = {
		.bck_o_en = -1,
		.ws_o_en = -1,
		.bck_i_en = -1,
		.ws_i_en = -1,
		.data_out_en = -1,
		.data_in_en = -1
	};

	if (ir_tx_gpio == GPIO_NUM_2)
		pin_config.ws_o_en = 1;
	else if (ir_tx_gpio == GPIO_NUM_14)
		pin_config.ws_i_en = 1;

	ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
	ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
}

static void ir_hw_timer_init(void)
{
	ESP_ERROR_CHECK(hw_timer_init(ir_tx_handler, NULL));
	ESP_ERROR_CHECK(hw_timer_disarm());
}

void ir_ctrl_init(uint8_t rx_gpio, uint8_t tx_gpio, ir_rx_callback cb)
{
	int ret;

	/* only these two pins can be reused as an I2S */
	ESP_ERROR_CHECK(tx_gpio != GPIO_NUM_2 && tx_gpio != GPIO_NUM_14);

	ir_rx_gpio = rx_gpio;
	ir_tx_gpio = tx_gpio;
	ir_rx_cb = cb;

	/* create a queue to handle gpio event from isr */
	detail_queue = xQueueCreate(100, sizeof(uint16_t));
	ESP_ERROR_CHECK(detail_queue == NULL);

	/* start ir rx task */
	ret = xTaskCreate(ir_rx_task, "ir_rx_task", 2048,
			  NULL, tskIDLE_PRIORITY + 10, NULL);
	ESP_ERROR_CHECK(ret != pdPASS);

	ir_gpio_init();
	ir_i2s_init();
	ir_hw_timer_init();
	ir_tx_clear_carrier();
}
