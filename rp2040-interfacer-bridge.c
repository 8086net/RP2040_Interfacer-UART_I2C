// SPDX-License-Identifier: MIT
/*
 * Copyright 2021 Álvaro Fernández Rojas <noltari@gmail.com>
 * Copyright (c) 2022 Nicolai Electronics
 * Copyright (c) 2023 Chris Burton
 */

#include "bsp/board.h"
#include <hardware/irq.h>
#include <hardware/structs/sio.h>
#include <hardware/uart.h>
#include "hardware/i2c.h"
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <string.h>
#include <tusb.h>
#include <hardware/flash.h>
#include "serial.h"
#include "kernel_i2c_flags.h"

#if !defined(MIN)
#define MIN(a, b) ((a > b) ? b : a)
#endif /* MIN */

// might as well use our RAM
#define BUFFER_SIZE 2560

// activity LED on duration
#define LED_TICKER_COUNT 500

#define DEF_BIT_RATE 9600
#define DEF_STOP_BITS 1
#define DEF_PARITY 0
#define DEF_DATA_BITS 8

#define I2C_INST i2c1
#define I2C_SDA  26
#define I2C_SCL  27

#define POWER_LED 16

/* commands from USB, must e.g. match command ids in kernel driver */
#define CMD_ECHO       0
#define CMD_GET_FUNC   1
#define CMD_SET_DELAY  2
#define CMD_GET_STATUS 3
#define CMD_I2C_IO     4
#define CMD_I2C_BEGIN  1  // flag fo I2C_IO
#define CMD_I2C_END    2  // flag fo I2C_IO

const unsigned long i2c_func = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;

#define STATUS_IDLE        0
#define STATUS_ADDRESS_ACK 1
#define STATUS_ADDRESS_NAK 2

static uint8_t i2c_state = STATUS_IDLE;

uint8_t i2c_data[1024] = {0};

uint8_t led_i2c_pin = 17;
uint32_t led_i2c_ticker;

typedef struct {
	uart_inst_t *const inst;
	uint8_t tx_pin;
	uint8_t rx_pin;
	uint    sm;
	uint8_t led_act_pin;
} uart_id_t;

typedef struct {
	cdc_line_coding_t usb_lc;
	cdc_line_coding_t uart_lc;
	mutex_t lc_mtx;
	uint8_t uart_rx_buffer[BUFFER_SIZE];
	uint32_t uart_rx_pos;
	uint8_t uart_to_usb_buffer[BUFFER_SIZE];
	uint32_t uart_to_usb_pos;
	mutex_t uart_mtx;
	uint8_t usb_to_uart_buffer[BUFFER_SIZE];
	uint32_t usb_to_uart_pos;
	uint32_t usb_to_uart_snd;
	mutex_t usb_mtx;
	uint32_t led_act_ticker;
} uart_data_t;

uart_id_t UART_ID[CFG_TUD_CDC] = {
	{
		.inst = uart0,
		.tx_pin = 12,
		.rx_pin = 13,
		.led_act_pin = 14,
	}
};

uart_data_t UART_DATA[CFG_TUD_CDC];

uint rx_offset=0;
uint rxp_offset=0;

uint tx_offset=0;
uint txp_offset=0;

static inline uint databits_usb2uart(uint8_t data_bits)
{
	switch (data_bits) {
		case 5:
			return 5;
		case 6:
			return 6;
		case 7:
			return 7;
		default:
			return 8;
	}
}

static inline uart_parity_t parity_usb2uart(uint8_t usb_parity)
{
	switch (usb_parity) {
		case 1:
			return UART_PARITY_ODD;
		case 2:
			return UART_PARITY_EVEN;
		default:
			return UART_PARITY_NONE;
	}
}

static inline uint stopbits_usb2uart(uint8_t stop_bits)
{
	switch (stop_bits) {
		case 2:
			return 2;
		default:
			return 1;
	}
}

void update_uart_cfg(uint8_t itf)
{
	uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	if (mutex_try_enter(&ud->lc_mtx, NULL)) {
		if (ud->usb_lc.bit_rate != ud->uart_lc.bit_rate) {
			uart_set_baudrate(ui->inst, ud->usb_lc.bit_rate);
			ud->uart_lc.bit_rate = ud->usb_lc.bit_rate;
		}

		if ((ud->usb_lc.stop_bits != ud->uart_lc.stop_bits) ||
		   (ud->usb_lc.parity != ud->uart_lc.parity) ||
		   (ud->usb_lc.data_bits != ud->uart_lc.data_bits)) {
			uart_set_format(ui->inst,
				databits_usb2uart(ud->usb_lc.data_bits),
				stopbits_usb2uart(ud->usb_lc.stop_bits),
				parity_usb2uart(ud->usb_lc.parity));
			ud->uart_lc.data_bits = ud->usb_lc.data_bits;
			ud->uart_lc.parity = ud->usb_lc.parity;
			ud->uart_lc.stop_bits = ud->usb_lc.stop_bits;
		}
		mutex_exit(&ud->lc_mtx);
	}
}

void usb_read_bytes(uint8_t itf) {
	uint32_t len = tud_cdc_n_available(itf);

	if (len) {
		uart_data_t *ud = &UART_DATA[itf];

		mutex_enter_blocking(&ud->usb_mtx);

		len = MIN(len, BUFFER_SIZE - ud->usb_to_uart_pos);
		if (len) {
			uint32_t count;

			count = tud_cdc_n_read(itf, &ud->usb_to_uart_buffer[ud->usb_to_uart_pos], len);
			ud->usb_to_uart_pos += count;
		}

		mutex_exit(&ud->usb_mtx);
	}
}

void usb_write_bytes(uint8_t itf) {
	uart_data_t *ud = &UART_DATA[itf];

	if (ud->uart_to_usb_pos && mutex_try_enter(&ud->uart_mtx, NULL)) {
		uint32_t count;

		count = tud_cdc_n_write(itf, ud->uart_to_usb_buffer, ud->uart_to_usb_pos);
		if (count < ud->uart_to_usb_pos)
			memcpy(ud->uart_to_usb_buffer, &ud->uart_to_usb_buffer[count],
			      ud->uart_to_usb_pos - count);
		ud->uart_to_usb_pos -= count;

		mutex_exit(&ud->uart_mtx);

		if (count)
			tud_cdc_n_write_flush(itf);
	}
}

void usb_cdc_process(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	mutex_enter_blocking(&ud->lc_mtx);
	tud_cdc_n_get_line_coding(itf, &ud->usb_lc);
	mutex_exit(&ud->lc_mtx);

	usb_read_bytes(itf);
	usb_write_bytes(itf);
}

void core1_entry(void)
{
	tusb_init();

	while (1) {
		int itf;

		tud_task();

		for (itf = 0; itf < CFG_TUD_CDC; itf++) {
			if (tud_cdc_n_connected(itf)) {
				usb_cdc_process(itf);
			}
		}

		if (led_i2c_ticker) {
			gpio_put(led_i2c_pin, 1);
			led_i2c_ticker--;
		} else {
			gpio_put(led_i2c_pin, 0);
		}
	}
}

void uart_read_bytes(uint8_t itf)
{
	const uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	if (uart_is_readable(ui->inst)) {
		while (uart_is_readable(ui->inst) &&
				ud->uart_rx_pos < BUFFER_SIZE) {
			ud->uart_rx_buffer[ud->uart_rx_pos] = uart_getc(ui->inst);
			ud->uart_rx_pos++;
			ud->led_act_ticker = LED_TICKER_COUNT;
		}
	}

	// If we can get the uart mutex then copy the UART data to the uart USB sender, otherwise we'll get it next time around
	if (mutex_try_enter(&ud->uart_mtx, NULL)) {
		// Ensure we don't overflow the uart_to_usb_buffer
		uint32_t len = MIN(ud->uart_rx_pos, BUFFER_SIZE - ud->uart_to_usb_pos);
		memcpy(&ud->uart_to_usb_buffer[ud->uart_to_usb_pos], ud->uart_rx_buffer, len);
		ud->uart_to_usb_pos += len;
		ud->uart_rx_pos = 0;
		mutex_exit(&ud->uart_mtx);
	}

	if (ud->led_act_ticker) {
		gpio_put(ui->led_act_pin, 1);
		ud->led_act_ticker--;
	} else {
		gpio_put(ui->led_act_pin, 0);
	}
}

void uart_write_bytes(uint8_t itf) {
	const uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	// Try to get the usb_mutex and don't block if we cannot get it, we'll TX the data next passs
	if ((ud->usb_to_uart_pos) && (ud->usb_to_uart_snd < ud->usb_to_uart_pos) &&
	    mutex_try_enter(&ud->usb_mtx, NULL)) {
		const uart_id_t *ui = &UART_ID[itf];

		ud->led_act_ticker = LED_TICKER_COUNT;

		while (uart_is_writable(ui->inst)&&(ud->usb_to_uart_snd < ud->usb_to_uart_pos)) {
			uart_putc(ui->inst, ud->usb_to_uart_buffer[ud->usb_to_uart_snd++]);
		}

		// only reset buffers if we've sent everything
		if (ud->usb_to_uart_snd == ud->usb_to_uart_pos) {
			ud->usb_to_uart_pos = 0;
			ud->usb_to_uart_snd = 0;
		}
		mutex_exit(&ud->usb_mtx);
	}
	if (ud->led_act_ticker) {
		gpio_put(ui->led_act_pin, 1);
		ud->led_act_ticker--;
	} else {
		gpio_put(ui->led_act_pin, 0);
	}
}

static inline void init_usb_cdc_serial_num() {
	uint8_t id[8];
	flash_get_unique_id(id);
	for (int i = 0; i < 8; ++i) {
		sprintf(serial + 2 * i, "%X", id[i]);
	}
	serial[16] = '\0';
}

void init_uart_data(uint8_t itf) {
	uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	/* Pinmux */
	gpio_set_function(ui->tx_pin, GPIO_FUNC_UART);
	gpio_set_function(ui->rx_pin, GPIO_FUNC_UART);

	/* USB CDC LC */
	ud->usb_lc.bit_rate = DEF_BIT_RATE;
	ud->usb_lc.data_bits = DEF_DATA_BITS;
	ud->usb_lc.parity = DEF_PARITY;
	ud->usb_lc.stop_bits = DEF_STOP_BITS;

	/* UART LC */
	ud->uart_lc.bit_rate = DEF_BIT_RATE;
	ud->uart_lc.data_bits = DEF_DATA_BITS;
	ud->uart_lc.parity = DEF_PARITY;
	ud->uart_lc.stop_bits = DEF_STOP_BITS;

	/* Buffer */
	ud->uart_rx_pos = 0;
	ud->uart_to_usb_pos = 0;
	ud->usb_to_uart_pos = 0;
	ud->usb_to_uart_snd = 0;

	/* Mutex */
	mutex_init(&ud->lc_mtx);
	mutex_init(&ud->uart_mtx);
	mutex_init(&ud->usb_mtx);

	/* Activity LED */
	gpio_init(ui->led_act_pin);
	gpio_set_dir(ui->led_act_pin, GPIO_OUT);
	gpio_put(ui->led_act_pin, 0);
	ud->led_act_ticker = 0;

	/* UART start */
	uart_init(ui->inst, ud->usb_lc.bit_rate);
	uart_set_hw_flow(ui->inst, false, false);
	uart_set_format(ui->inst, databits_usb2uart(ud->usb_lc.data_bits),
	stopbits_usb2uart(ud->usb_lc.stop_bits),
	parity_usb2uart(ud->usb_lc.parity));
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
	led_i2c_ticker = LED_TICKER_COUNT;
        switch (request->bRequest) {
            case CMD_ECHO:
                if (stage != CONTROL_STAGE_SETUP) return true;
                return tud_control_xfer(rhport, request, (void*) &request->wValue, sizeof(request->wValue));
            case CMD_GET_FUNC:
                if (stage != CONTROL_STAGE_SETUP) return true;
                return tud_control_xfer(rhport, request, (void*) &i2c_func, sizeof(i2c_func));
                break;
            case CMD_SET_DELAY:
                if (stage != CONTROL_STAGE_SETUP) return true;
                if (request->wValue == 0) {
                    i2c_set_baudrate(I2C_INST, 100000);  // Use default: 100kHz
                } else {
                    int baudrate = 1000000 / request->wValue;
                    if (baudrate > 400000) baudrate = 400000;  // Limit to 400kHz
                    i2c_set_baudrate(I2C_INST, baudrate);
                }
                return tud_control_status(rhport, request);
            case CMD_GET_STATUS:
                if (stage != CONTROL_STAGE_SETUP) return true;
                return tud_control_xfer(rhport, request, (void*) &i2c_state, sizeof(i2c_state));
            case CMD_I2C_IO:
            case CMD_I2C_IO + CMD_I2C_BEGIN:
            case CMD_I2C_IO + CMD_I2C_END:
            case CMD_I2C_IO + CMD_I2C_BEGIN + CMD_I2C_END:
                {
                    if (stage != CONTROL_STAGE_SETUP && stage != CONTROL_STAGE_DATA) return true;
                    bool nostop = !(request->bRequest & CMD_I2C_END);

                    //sprintf(buffer, "%s i2c %s at 0x%02x, len = %d, nostop = %d\r\n", (stage != CONTROL_STAGE_SETUP) ? "[D]" : "[S]", (request->wValue & I2C_M_RD)?"rd":"wr", request->wIndex, request->wLength, nostop);
                    //debug_print(buffer);

                    if (request->wLength > sizeof(i2c_data)) {
                        return false;  // Prevent buffer overflow in case host sends us an impossible request
                    }

                    if (stage == CONTROL_STAGE_SETUP) {  // Before transfering data
                        if (request->wValue & I2C_M_RD) {
                            // Reading from I2C device
                            int res = i2c_read_blocking(I2C_INST, request->wIndex, i2c_data, request->wLength, nostop);
                            if (res == PICO_ERROR_GENERIC) {
                                i2c_state = STATUS_ADDRESS_NAK;
                            } else {
                                i2c_state = STATUS_ADDRESS_ACK;
                            }
                        } else if (request->wLength == 0) {  // Writing with length of 0, this is used for bus scanning, do dummy read
                            uint8_t dummy = 0x00;
                            int     res   = i2c_read_blocking(I2C_INST, request->wIndex, (void*) &dummy, 1, nostop);
                            if (res == PICO_ERROR_GENERIC) {
                                i2c_state = STATUS_ADDRESS_NAK;
                            } else {
                                i2c_state = STATUS_ADDRESS_ACK;
                            }
                        }
                        tud_control_xfer(rhport, request, (void*) i2c_data, request->wLength);
                    }

                    if (stage == CONTROL_STAGE_DATA) {        // After transfering data
                        if (!(request->wValue & I2C_M_RD)) {  // I2C write operation
                            int res = i2c_write_blocking(I2C_INST, request->wIndex, i2c_data, request->wLength, nostop);
                            if (res == PICO_ERROR_GENERIC) {
                                i2c_state = STATUS_ADDRESS_NAK;
                            } else {
                                i2c_state = STATUS_ADDRESS_ACK;
                            }
                        }
                    }

                    return true;
                }
            default:
                if (stage != CONTROL_STAGE_SETUP) return true;
                break;
        }
    } else {
        if (stage != CONTROL_STAGE_SETUP) return true;
    }

    return false;  // stall unknown request
}

bool tud_vendor_control_complete_cb(uint8_t rhport, tusb_control_request_t const* request) {
    (void) rhport;
    (void) request;
    return true;
}

int main(void)
{
	int itf;

	board_init();

	gpio_init(POWER_LED);
	gpio_set_dir(POWER_LED, GPIO_OUT);
	gpio_put(POWER_LED, 1);

	gpio_init(I2C_SDA);
	gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
	gpio_pull_up(I2C_SDA);

	gpio_init(I2C_SCL);
	gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
	gpio_pull_up(I2C_SCL);

	i2c_init(I2C_INST, 100000);

	gpio_init(led_i2c_pin);
	gpio_set_dir(led_i2c_pin, GPIO_OUT);
	gpio_put(led_i2c_pin, 0);
	led_i2c_ticker = 0;

	init_usb_cdc_serial_num();

	for (itf = 0; itf < CFG_TUD_CDC; itf++)
		init_uart_data(itf);

	multicore_launch_core1(core1_entry);

	while (1) {
		for (itf = 0; itf < CFG_TUD_CDC; itf++) {
			update_uart_cfg(itf);
			uart_read_bytes(itf);
			uart_write_bytes(itf);
		}
	}

	return 0;
}

