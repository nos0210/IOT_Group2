#ifndef HTTP_CLIENT_TASK_H
#define HTTP_CLIENT_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task: DNS lookup, TCP connect, send GET / HTTP/1.1
 *        to httpforever.com:80 via coreHTTP, then log the response.
 *
 * Stack requirement: at least 8 KB (response buffer is 8 KB on stack).
 * Priority: any reasonable value (5 recommended).
 */
void http_client_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_TASK_H */
