#include "network_service.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define UDP_PORT                       5005
#define TCP_PORT                       5006
#define NET_BUF_SIZE                   256
#define NET_BASE_RETRY_DELAY_MS        500
#define NET_MAX_RETRY_DELAY_MS         4000
#define NET_SEND_RETRY_COUNT           3
#define NET_TASK_STACK                 4096
#define NET_TASK_PRIORITY              5
#define NET_MANAGER_POLL_MS            300
#define UDP_HEARTBEAT_MS               5000

#define BIT_WIFI_UP                    BIT0

static const char *TAG = "NET_SVC";

static EventGroupHandle_t g_net_event_group;
static TaskHandle_t g_manager_task;
static TaskHandle_t g_udp_task;
static TaskHandle_t g_tcp_task;

static volatile bool g_udp_running;
static volatile bool g_tcp_running;

static int g_udp_sock = -1;
static int g_tcp_listen_sock = -1;
static int g_tcp_client_sock = -1;

static void network_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_net_event_group, BIT_WIFI_UP);
        ESP_LOGI(TAG, "Wi-Fi up -> network workers allowed");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_net_event_group, BIT_WIFI_UP);
        ESP_LOGW(TAG, "Wi-Fi down -> network workers reconnect later");
    }
}

static bool net_is_wifi_up(void)
{
    EventBits_t bits = xEventGroupGetBits(g_net_event_group);
    return (bits & BIT_WIFI_UP) != 0;
}

static int net_retry_delay_ms(int attempt)
{
    int delay = NET_BASE_RETRY_DELAY_MS << attempt;
    if (delay > NET_MAX_RETRY_DELAY_MS) {
        delay = NET_MAX_RETRY_DELAY_MS;
    }
    return delay;
}

static void net_close_socket(int *sock)
{
    if (sock == NULL || *sock < 0) {
        return;
    }

    shutdown(*sock, SHUT_RDWR);
    close(*sock);
    *sock = -1;
}

static int net_send_retry(int sock, const char *data, size_t len)
{
    for (int attempt = 0; attempt < NET_SEND_RETRY_COUNT; ++attempt) {
        int sent = send(sock, data, len, 0);
        if (sent >= 0) {
            return sent;
        }

        int err = errno;
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(attempt)));
            continue;
        }

        return -1;
    }

    return -1;
}

static int net_sendto_retry(int sock, const char *data, size_t len, const struct sockaddr_in *dest)
{
    for (int attempt = 0; attempt < NET_SEND_RETRY_COUNT; ++attempt) {
        int sent = sendto(sock, data, len, 0, (const struct sockaddr *)dest, sizeof(*dest));
        if (sent >= 0) {
            return sent;
        }

        int err = errno;
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(attempt)));
            continue;
        }

        return -1;
    }

    return -1;
}

static void build_udp_response(const char *request, char *response, size_t response_len)
{
    if (strncmp(request, "PING", 4) == 0) {
        snprintf(response, response_len, "PONG");
        return;
    }

    snprintf(response, response_len, "ACK:%s", request);
}

static void udp_worker_task(void *arg)
{
    (void)arg;

    char rx_buf[NET_BUF_SIZE];
    char tx_buf[NET_BUF_SIZE];

    bool has_peer = false;
    struct sockaddr_in last_peer = {0};
    TickType_t last_heartbeat = xTaskGetTickCount();

    int bind_attempt = 0;

    while (g_udp_running) {
        if (!net_is_wifi_up()) {
            net_close_socket(&g_udp_sock);
            has_peer = false;
            bind_attempt = 0;
            vTaskDelay(pdMS_TO_TICKS(NET_MANAGER_POLL_MS));
            continue;
        }

        if (g_udp_sock < 0) {
            g_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (g_udp_sock < 0) {
                ESP_LOGE(TAG, "UDP socket create failed errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(bind_attempt++)));
                continue;
            }

            struct timeval timeout = {
                .tv_sec = 1,
                .tv_usec = 0,
            };
            setsockopt(g_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            struct sockaddr_in local_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(UDP_PORT),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };

            if (bind(g_udp_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
                ESP_LOGE(TAG, "UDP bind failed errno=%d", errno);
                net_close_socket(&g_udp_sock);
                vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(bind_attempt++)));
                continue;
            }

            bind_attempt = 0;
            ESP_LOGI(TAG, "UDP ready on port %d", UDP_PORT);
        }

        struct sockaddr_in from_addr = {0};
        socklen_t from_len = sizeof(from_addr);
        int rx_len = recvfrom(g_udp_sock, rx_buf, sizeof(rx_buf) - 1, 0, (struct sockaddr *)&from_addr, &from_len);

        if (rx_len > 0) {
            rx_buf[rx_len] = '\0';
            last_peer = from_addr;
            has_peer = true;

            build_udp_response(rx_buf, tx_buf, sizeof(tx_buf));
            if (net_sendto_retry(g_udp_sock, tx_buf, strlen(tx_buf), &from_addr) < 0) {
                ESP_LOGW(TAG, "UDP response send failed errno=%d", errno);
            }
        } else if (rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "UDP recv error errno=%d -> recreate socket", errno);
            net_close_socket(&g_udp_sock);
            vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(0)));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if (has_peer && (now - last_heartbeat) >= pdMS_TO_TICKS(UDP_HEARTBEAT_MS)) {
            const char *heartbeat = "UDP_HEARTBEAT";
            if (net_sendto_retry(g_udp_sock, heartbeat, strlen(heartbeat), &last_peer) < 0) {
                ESP_LOGW(TAG, "UDP heartbeat send failed errno=%d", errno);
            }
            last_heartbeat = now;
        }
    }

    net_close_socket(&g_udp_sock);
    g_udp_task = NULL;
    ESP_LOGI(TAG, "UDP worker stopped");
    vTaskDelete(NULL);
}

static void build_tcp_response(const char *request, char *response, size_t response_len)
{
    if (strncmp(request, "PING", 4) == 0) {
        snprintf(response, response_len, "PONG\\n");
        return;
    }

    if (strncmp(request, "GET_STATUS", 10) == 0) {
        snprintf(response, response_len, "WIFI_UP\\n");
        return;
    }

    snprintf(response, response_len, "ACK:%s\\n", request);
}

static int tcp_create_listen_socket(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        net_close_socket(&sock);
        return -1;
    }

    if (listen(sock, 1) < 0) {
        net_close_socket(&sock);
        return -1;
    }

    return sock;
}

static void tcp_worker_task(void *arg)
{
    (void)arg;

    char rx_buf[NET_BUF_SIZE];
    char tx_buf[NET_BUF_SIZE];

    int listen_attempt = 0;

    while (g_tcp_running) {
        if (!net_is_wifi_up()) {
            net_close_socket(&g_tcp_client_sock);
            net_close_socket(&g_tcp_listen_sock);
            listen_attempt = 0;
            vTaskDelay(pdMS_TO_TICKS(NET_MANAGER_POLL_MS));
            continue;
        }

        if (g_tcp_listen_sock < 0) {
            g_tcp_listen_sock = tcp_create_listen_socket();
            if (g_tcp_listen_sock < 0) {
                ESP_LOGE(TAG, "TCP listen socket create failed errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(listen_attempt++)));
                continue;
            }

            listen_attempt = 0;
            ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(g_tcp_listen_sock, &read_fds);

        struct timeval accept_timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };

        int sel = select(g_tcp_listen_sock + 1, &read_fds, NULL, NULL, &accept_timeout);
        if (sel < 0) {
            ESP_LOGW(TAG, "TCP select failed errno=%d", errno);
            net_close_socket(&g_tcp_listen_sock);
            vTaskDelay(pdMS_TO_TICKS(net_retry_delay_ms(0)));
            continue;
        }

        if (sel == 0 || !FD_ISSET(g_tcp_listen_sock, &read_fds)) {
            continue;
        }

        struct sockaddr_in client_addr = {0};
        socklen_t client_len = sizeof(client_addr);
        g_tcp_client_sock = accept(g_tcp_listen_sock, (struct sockaddr *)&client_addr, &client_len);
        if (g_tcp_client_sock < 0) {
            ESP_LOGW(TAG, "TCP accept failed errno=%d", errno);
            continue;
        }

        ESP_LOGI(TAG, "TCP client connected");

        struct timeval recv_timeout = {
            .tv_sec = 2,
            .tv_usec = 0,
        };
        setsockopt(g_tcp_client_sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        while (g_tcp_running && net_is_wifi_up()) {
            int rx_len = recv(g_tcp_client_sock, rx_buf, sizeof(rx_buf) - 1, 0);
            if (rx_len > 0) {
                rx_buf[rx_len] = '\0';
                build_tcp_response(rx_buf, tx_buf, sizeof(tx_buf));
                if (net_send_retry(g_tcp_client_sock, tx_buf, strlen(tx_buf)) < 0) {
                    ESP_LOGW(TAG, "TCP send failed errno=%d", errno);
                    break;
                }
            } else if (rx_len == 0) {
                ESP_LOGI(TAG, "TCP client disconnected");
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                ESP_LOGW(TAG, "TCP recv error errno=%d", errno);
                break;
            }
        }

        net_close_socket(&g_tcp_client_sock);
    }

    net_close_socket(&g_tcp_client_sock);
    net_close_socket(&g_tcp_listen_sock);
    g_tcp_task = NULL;
    ESP_LOGI(TAG, "TCP worker stopped");
    vTaskDelete(NULL);
}

static void network_manager_task(void *arg)
{
    (void)arg;

    bool workers_started = false;

    while (1) {
        bool wifi_up = net_is_wifi_up();

        if (wifi_up && !workers_started) {
            g_udp_running = true;
            g_tcp_running = true;

            BaseType_t udp_ok = xTaskCreate(udp_worker_task, "udp_worker", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, &g_udp_task);
            BaseType_t tcp_ok = xTaskCreate(tcp_worker_task, "tcp_worker", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, &g_tcp_task);

            if (udp_ok != pdPASS || tcp_ok != pdPASS) {
                ESP_LOGE(TAG, "Failed to create network workers");
                g_udp_running = false;
                g_tcp_running = false;
                if (udp_ok == pdPASS) {
                    while (g_udp_task != NULL) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                if (tcp_ok == pdPASS) {
                    while (g_tcp_task != NULL) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            workers_started = true;
            ESP_LOGI(TAG, "Network workers started");
        }

        if (!wifi_up && workers_started) {
            g_udp_running = false;
            g_tcp_running = false;
            net_close_socket(&g_udp_sock);
            net_close_socket(&g_tcp_client_sock);
            net_close_socket(&g_tcp_listen_sock);

            while (g_udp_task != NULL || g_tcp_task != NULL) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            workers_started = false;
            ESP_LOGI(TAG, "Network workers stopped, waiting Wi-Fi recovery");
        }

        vTaskDelay(pdMS_TO_TICKS(NET_MANAGER_POLL_MS));
    }
}

esp_err_t network_service_init(void)
{
    if (g_net_event_group != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    g_net_event_group = xEventGroupCreate();
    if (g_net_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event_handler, NULL),
        TAG,
        "register WIFI_EVENT failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event_handler, NULL),
        TAG,
        "register IP_EVENT failed"
    );

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        xEventGroupSetBits(g_net_event_group, BIT_WIFI_UP);
    }

    BaseType_t task_ok = xTaskCreate(network_manager_task, "net_manager", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, &g_manager_task);
    if (task_ok != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Network service initialized");
    return ESP_OK;
}
