idf_component_register(SRCS "main.c" "desk_protocol.c" "desk_diagnostics.c" "desk_tracking.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart esp_timer)
