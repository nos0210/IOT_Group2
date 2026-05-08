# ── coreHTTP + llhttp source paths (relative to this CMakeLists.txt) ──────
set(COREHTTP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../coreHTTP")
set(LLHTTP_ROOT   "${COREHTTP_ROOT}/source/dependency/3rdparty/llhttp")

set(COREHTTP_SRCS
    "${COREHTTP_ROOT}/source/core_http_client.c"
    "${LLHTTP_ROOT}/src/api.c"
    "${LLHTTP_ROOT}/src/llhttp.c"
    "${LLHTTP_ROOT}/src/http.c"
)

set(COREHTTP_INCLUDES
    "${COREHTTP_ROOT}/source/include"
    "${COREHTTP_ROOT}/source/interface"
    "${LLHTTP_ROOT}/include"
)

# ── coreMQTT source paths ──────────────────────────────────────────────────
set(COREMQTT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../aws-iot-device-sdk-embedded-C/libraries/standard/coreMQTT")

set(COREMQTT_SRCS
    "${COREMQTT_ROOT}/source/core_mqtt.c"
    "${COREMQTT_ROOT}/source/core_mqtt_serializer.c"
    "${COREMQTT_ROOT}/source/core_mqtt_state.c"
)

set(COREMQTT_INCLUDES
    "${COREMQTT_ROOT}/source/include"
    "${COREMQTT_ROOT}/source/interface"
)

idf_component_register(
    SRCS
        "main.c"
        "../modules/ADC_DAC.c"
        "../modules/wifi_ble_provision.c"
        "../modules/network_service.c"
        "../modules/http_client_task.c"
        "../modules/mqtt_client_task.c"
        ${COREHTTP_SRCS}
        ${COREMQTT_SRCS}
    INCLUDE_DIRS
        "."
        "../include"
        ${COREHTTP_INCLUDES}
        ${COREMQTT_INCLUDES}
    REQUIRES
        esp_adc esp_driver_dac esp_driver_gpio
        esp_wifi esp_event nvs_flash bt lwip esp_netif esp_timer
)

# Suppress -Werror=unused-value for coreMQTT third-party sources
# (coreMQTT uses comma-expression style logging macros that trigger this warning)
set_source_files_properties(
    "${COREMQTT_ROOT}/source/core_mqtt.c"
    "${COREMQTT_ROOT}/source/core_mqtt_serializer.c"
    "${COREMQTT_ROOT}/source/core_mqtt_state.c"
    PROPERTIES COMPILE_FLAGS "-Wno-unused-value"
)
