#ifndef AWS_IOT_CONFIG_H
#define AWS_IOT_CONFIG_H

/*
 * aws_iot_config.h — Cấu hình AWS IoT Core cho ESP32
 *
 * ⚠️  ĐIỀN THÔNG TIN CỦA BẠN VÀO ĐÂY trước khi build:
 *   1. AWS_IOT_ENDPOINT  : lấy từ lệnh
 *                          aws iot describe-endpoint --endpoint-type iot:Data-ATS
 *   2. AWS_DEVICE_ID     : tên Thing đã tạo trên AWS IoT
 */

/* ── AWS IoT Core endpoint ────────────────────────────────────────────────── */
#define AWS_IOT_ENDPOINT   "adiub9wc49ljw-ats.iot.ap-southeast-1.amazonaws.com"
#define AWS_IOT_PORT        8883

/* ── Device identity ──────────────────────────────────────────────────────── */
#define AWS_DEVICE_ID      "esp32-001"

/* ── MQTT Topics ──────────────────────────────────────────────────────────── */
#define TOPIC_OTA_CMD    "devices/" AWS_DEVICE_ID "/ota/command"
#define TOPIC_OTA_STATUS "devices/" AWS_DEVICE_ID "/ota/status"

#endif /* AWS_IOT_CONFIG_H */
