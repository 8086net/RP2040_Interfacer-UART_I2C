# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.13)

include(pico-sdk/pico_sdk_init.cmake)

project(rp2040_interfacer_bridge)

pico_sdk_init()
  
add_executable(rp2040_interfacer_bridge rp2040-interfacer-bridge.c usb-descriptors.c)

add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(rp2040_interfacer_bridge PUBLIC
	./
	../pico-sdk/lib/tinyusb/src)

target_link_libraries(rp2040_interfacer_bridge
	pico_multicore
	pico_stdlib
	hardware_flash
	hardware_i2c
	tinyusb_device
	tinyusb_board)

pico_add_extra_outputs(rp2040_interfacer_bridge)
