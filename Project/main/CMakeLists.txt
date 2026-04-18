idf_component_register(SRCS "main.c" "../modules/ADC_DAC.c" "../modules/wifi_ble_provision.c"
                    INCLUDE_DIRS "." "../include"
                    REQUIRES esp_adc esp_driver_dac esp_driver_gpio esp_wifi esp_event nvs_flash bt)
