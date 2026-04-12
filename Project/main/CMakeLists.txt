idf_component_register(SRCS "main.c" "../modules/ADC_DAC.c"
                    INCLUDE_DIRS "." "../include"
                    REQUIRES esp_adc driver)
