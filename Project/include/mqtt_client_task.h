#ifndef MQTT_CLIENT_TASK_H
#define MQTT_CLIENT_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task: DNS lookup, TCP connect to test.mosquitto.org:1883,
 *        MQTT CONNECT → SUBSCRIBE → PUBLISH loop using coreMQTT.
 *
 * Topics:
 *   Subscribe : esp32/from_mqttx  (receive messages from MQTTX on Windows)
 *   Publish   : esp32/to_mqttx    (send sensor data to MQTTX on Windows)
 *
 * Stack requirement: at least 8 KB.
 * Priority: 5 recommended.
 */
void mqtt_client_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_TASK_H */
