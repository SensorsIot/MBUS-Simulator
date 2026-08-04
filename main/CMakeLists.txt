# PRIV_REQUIRES, not REQUIRES: main exposes no headers of its own, so nothing
# downstream inherits these. ESP-IDF 6 bundles no catch-all component, so each
# driver has to be named individually.
idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES e450_core
                                     esp_driver_uart
                                     esp_driver_gpio
                                     esp_timer
                                     console
                                     vfs)
