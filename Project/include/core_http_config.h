/*
 * coreHTTP configuration for ESP32 plain HTTP client.
 */
#ifndef CORE_HTTP_CONFIG_H
#define CORE_HTTP_CONFIG_H

/* Maximum size of the response headers buffer (bytes). */
#define HTTP_MAX_RESPONSE_HEADERS_SIZE_BYTES    2048U

/* User-Agent header value sent with every request. */
#define HTTP_USER_AGENT_VALUE                   "ESP32-coreHTTP/1.0"

/* Timeout (ms) when retrying a network send that returned 0 bytes. */
#define HTTP_SEND_RETRY_TIMEOUT_MS              3000U

/* Timeout (ms) when retrying a network recv that returned 0 bytes. */
#define HTTP_RECV_RETRY_TIMEOUT_MS              3000U

#endif /* CORE_HTTP_CONFIG_H */
