idf_component_register(SRCS "src/provision.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_driver_usb_serial_jtag esp_timer esp_system app_update settings storage motor selftest sound)
