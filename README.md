RP2040 Interfacer USB-UART and I2C Bridge
=================================
This project provides a UART and I2C interface on the 8086.net "RP2040 Interfacer" board.

Supporting hardware, the "RP2040 Interfacer" is available on [Tindie](https://www.tindie.com/products/36879/).

The USB UART is compatible with the builtin USB CDC class drivers in Linux, macOS, and >= Windows 10.

The I2C interface requires an i2c-tiny-usb driver. On Linux copy the "90-RP2040_Interfacer.rules" file into "/etc/udev/rules.d/", run "sudo udevadm control --reload" and then unplug/replug the RP2040 Interfacer. Running "i2cdetect -l" will then show the i2c-tiny-usb bus you need to use in your application.

History
----------

This is a cut down version of our [pico-sexa-uart-bridge](https://github.com/8086net/pico-sexa-uart-bridge) firmware (6xUART+I2C) which itself expands [JoeSc's](https://github.com/JoeSc/pico-sexa-uart-bridge) project to add activity LED for each UART which itself expands [Noltari's](https://github.com/Noltari/pico-uart-bridge) project to add 4 additional UARTs using the pico PIOs. And expands on [harrywalsh's](https://github.com/harrywalsh/pico-hw_and_pio-uart-gridge) project to provide better SEO and remove some data loss when using all 6 UARTs concurrently.

It then has [Nicolai-Electronics's](https://github.com/Nicolai-Electronics/rp2040-i2c-interface) project for i2c-tiny-usb support merged in.

Disclaimer
----------

This software is provided without warranty, according to the MIT License, and should therefore not be used where it may endanger life, financial stakes, or cause discomfort and inconvenience to others.

RP2040 Interfacer pinout
------------------------

| RP2040 Interfacer | Function |
|:----------------------:|:--------:|
| GPIO12          | UART0 TX |
| GPIO13          | UART0 RX |
| GPIO14          | Amber UART0 Activity LED |
| GPIO16          | Red Power LED |
| GPIO17          | Amber I2C Activity LED |
| GPIO18          | UNUSED (1K Resistor to SDA) |
| GPIO19          | UNUSED (1K Resistor to SCL) |
| GPIO23          | UNUSED (User Button) |
| GPIO26          | ST/Qw I2C SDA |
| GPIO27          | ST/Qw I2C SCL |
