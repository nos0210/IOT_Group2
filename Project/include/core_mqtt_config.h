/*
 * coreMQTT configuration for ESP32 plain MQTT client.
 * No dependency on AWS demo logging stack — uses ESP_LOG instead.
 */
#ifndef CORE_MQTT_CONFIG_H_
#define CORE_MQTT_CONFIG_H_

/* ── Logging shim ────────────────────────────────────────────────────────── */
/* coreMQTT calls logging as: LogDebug( ("format %d", value) )
 * The argument is a parenthesised tuple.  Using "printf message" expands it
 * to printf("format %d", value) which is valid C with no comma-expr warning. */
#include <stdio.h>

#define LogError( message )   do { printf( "[E] coreMQTT: " ); printf message; printf( "\r\n" ); } while(0)
#define LogWarn( message )    do { printf( "[W] coreMQTT: " ); printf message; printf( "\r\n" ); } while(0)
#define LogInfo( message )    do { printf( "[I] coreMQTT: " ); printf message; printf( "\r\n" ); } while(0)
#define LogDebug( message )   do { (void)0; } while(0)

/* ── MQTT library tunables ───────────────────────────────────────────────── */

/**
 * @brief Maximum number of pending PUBLISH acknowledgements (QoS 1/2)
 * for both incoming and outgoing directions.
 */
#define MQTT_STATE_ARRAY_MAX_COUNT          10U

/**
 * @brief Milliseconds to wait for a CONNACK before giving up.
 */
#define MQTT_SEND_TIMEOUT_MS                3000U

/**
 * @brief Milliseconds to wait for incoming data during ProcessLoop.
 */
#define MQTT_RECV_POLLING_TIMEOUT_MS        1000U

/**
 * @brief Maximum number of CONNACK receive retries.
 */
#define MQTT_MAX_CONNACK_RECEIVE_RETRY_COUNT  3U

#endif /* CORE_MQTT_CONFIG_H_ */
