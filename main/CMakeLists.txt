idf_component_register(SRCS "desk_button.c" "desk_button_logic.c" "main.c" "desk_protocol.c" "desk_diagnostics.c" "desk_tracking.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart esp_timer esp_driver_gpio esp_driver_gptimer esp_driver_usb_serial_jtag)
