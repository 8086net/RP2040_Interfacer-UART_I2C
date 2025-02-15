# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.13)

include(pico-sdk/pico_sdk_init.cmake)

project(usb_interfacer_bridge)

pico_sdk_init()
  
add_executable(usb_interfacer_bridge usb-interfacer-bridge.c usb-descriptors.c)

add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(usb_interfacer_bridge PUBLIC
	./
	../pico-sdk/lib/tinyusb/src)

target_link_libraries(usb_interfacer_bridge
	pico_multicore
	pico_stdlib
	hardware_flash
	hardware_i2c
	tinyusb_device
	tinyusb_board)

pico_add_extra_outputs(usb_interfacer_bridge)
