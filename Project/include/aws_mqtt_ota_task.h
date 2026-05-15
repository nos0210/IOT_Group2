#ifndef AWS_MQTT_OTA_TASK_H
#define AWS_MQTT_OTA_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task: kết nối tới AWS IoT Core qua TLS (port 8883) với
 *        mutual X.509 authentication, subscribe topic OTA command, thực hiện
 *        cập nhật firmware qua esp_https_ota khi nhận lệnh, publish trạng
 *        thái OTA về AWS IoT Core.
 *
 * Yêu cầu:
 *   - Điền AWS_IOT_ENDPOINT và AWS_DEVICE_ID trong aws_iot_config.h
 *   - Certificates đã có trong certs/  (AmazonRootCA1.pem, device.crt, device.key)
 *   - Partition table OTA (partitions_ota.csv)
 *
 * Stack: tối thiểu 8 KB, khuyến nghị 16 KB (esp_https_ota cần stack lớn).
 * Priority: 5.
 */
void aws_mqtt_ota_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* AWS_MQTT_OTA_TASK_H */
