/*
 * aws_mqtt_ota_task.c
 *
 * AWS IoT Core MQTT task với OTA update qua esp_https_ota.
 *
 * Luồng hoạt động:
 *   1. Chờ Wi-Fi có IP
 *   2. Kết nối TLS tới AWS IoT Core (port 8883) dùng mutual X.509
 *   3. MQTT CONNECT + SUBSCRIBE "devices/<id>/ota/command"
 *   4. ProcessLoop — khi nhận OTA command:
 *        a. Publish "downloading" status
 *        b. Ngắt MQTT + TLS
 *        c. Gọi esp_https_ota(presigned S3 URL)
 *        d. Kết nối lại → Publish "success" hoặc "failed"
 *        e. Nếu thành công → esp_restart()
 *   5. Kết nối lại nếu mất kết nối
 */

#include "aws_mqtt_ota_task.h"
#include "aws_iot_config.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_rom_md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core_mqtt.h"

/* ── Minimal JSON string field extractor (no external library needed) ────── */
/**
 * @brief Find the value of a JSON string field, e.g. "key":"value".
 *        Handles basic escape sequences. NOT a full JSON parser.
 *        Only for trusted, well-formed payloads from AWS IoT Core.
 *
 * @return true if field was found and value is non-empty, false otherwise.
 */
static bool json_get_string_field(const char *json, size_t json_len,
                                   const char *key,
                                   char *out, size_t out_len)
{
    /* Build search pattern without the closing quote: "key": */
    char search_key[70];
    int klen = snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    if (klen <= 0 || (size_t)klen >= sizeof(search_key)) return false;

    /* Scan for the key pattern */
    const char *p = NULL;
    for (size_t i = 0; i + (size_t)klen <= json_len; i++) {
        if (memcmp(json + i, search_key, (size_t)klen) == 0) {
            /* Skip optional whitespace after colon, then expect opening quote */
            const char *q = json + i + (size_t)klen;
            size_t remaining_q = json_len - (size_t)(q - json);
            while (remaining_q > 0 && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) {
                q++; remaining_q--;
            }
            if (remaining_q > 0 && *q == '"') {
                p = q + 1;
                break;
            }
        }
    }
    if (!p) return false;

    /* Copy value until closing unescaped quote */
    size_t remaining = json_len - (size_t)(p - json);
    size_t n = 0;
    for (size_t i = 0; i < remaining && n < out_len - 1; i++) {
        if (p[i] == '"') break;
        if (p[i] == '\\' && i + 1 < remaining) {
            i++;
            out[n++] = p[i];
        } else {
            out[n++] = p[i];
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* ── Embedded certificates (nhúng từ certs/ qua CMake EMBED_TXTFILES) ─────── */
extern const uint8_t aws_root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t aws_root_ca_end[]   asm("_binary_AmazonRootCA1_pem_end");
extern const uint8_t device_crt_start[]  asm("_binary_device_crt_start");
extern const uint8_t device_crt_end[]    asm("_binary_device_crt_end");
extern const uint8_t device_key_start[]  asm("_binary_device_key_start");
extern const uint8_t device_key_end[]    asm("_binary_device_key_end");

/* ── Hằng số cấu hình ───────────────────────────────────────────────────────── */
#define MQTT_NETWORK_BUF_SIZE    4096U
#define MQTT_KEEP_ALIVE_SEC      60
#define MQTT_CONNECT_TIMEOUT_MS  10000U
#define MQTT_PROCESS_LOOP_MS     1000U
#define MQTT_RECONNECT_DELAY_MS  15000U
#define WIFI_WAIT_MS             1000U
#define WIFI_WAIT_TIMEOUT_MS     60000U
#define PENDING_ACK_COUNT        5U
#define OTA_URL_MAX_LEN          1024U
#define OTA_MD5_LEN              33U    /* 32 hex chars + null terminator */
#define OTA_MD5_READ_CHUNK       512U   /* bytes per flash read khi xác minh MD5 */

static const char *TAG = "AWS_OTA";

/* ── Trạng thái OTA chia sẻ giữa callback và task ───────────────────────────── */
static volatile bool s_ota_pending = false;
static char          s_ota_url[OTA_URL_MAX_LEN];
static char          s_ota_version[48];static char          s_ota_md5[OTA_MD5_LEN]; /* MD5 dự kiến từ MQTT command */
/* ── MQTT context (dùng trong publish helper) ───────────────────────────────── */
static MQTTContext_t s_mqtt_ctx;

/* ── NetworkContext cho coreMQTT TLS transport ──────────────────────────────── */
/* Phải khớp với forward declaration trong transport_interface.h của coreMQTT    */
struct NetworkContext {
    esp_tls_t *tls;
};
typedef struct NetworkContext NetworkContext_t;

/* ══════════════════════════════════════════════════════════════════════════════
 * Transport callbacks
 * ══════════════════════════════════════════════════════════════════════════════ */

static int32_t tls_transport_recv(NetworkContext_t *pCtx,
                                   void             *pBuffer,
                                   size_t            bytesToRecv)
{
    ssize_t n = esp_tls_conn_read(pCtx->tls, pBuffer, bytesToRecv);
    if (n > 0)  return (int32_t)n;
    if (n == 0) return -1;  /* peer closed */

    /* ESP_TLS_ERR_SSL_WANT_READ: SO_RCVTIMEO fired, không có dữ liệu — báo 0
     * để coreMQTT ProcessLoop tiếp tục polling. */
    if (n == ESP_TLS_ERR_SSL_WANT_READ) return 0;
    /* Kiểm tra thêm errno cho trường hợp socket timeout */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;

    ESP_LOGE(TAG, "TLS recv error: %d (errno=%d)", (int)n, errno);
    return -1;
}

static int32_t tls_transport_send(NetworkContext_t *pCtx,
                                   const void       *pBuffer,
                                   size_t            bytesToSend)
{
    ssize_t n = esp_tls_conn_write(pCtx->tls, pBuffer, bytesToSend);
    if (n < 0) {
        ESP_LOGE(TAG, "TLS send error: %d", (int)n);
        return -1;
    }
    return (int32_t)n;
}

/* Clock helper yêu cầu bởi coreMQTT */
static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * TLS connect / disconnect
 * ══════════════════════════════════════════════════════════════════════════════ */

static esp_tls_t *tls_connect(const char *host, int port)
{
    esp_tls_cfg_t cfg = {
        .cacert_buf       = aws_root_ca_start,
        .cacert_bytes     = (unsigned)(aws_root_ca_end - aws_root_ca_start),
        .clientcert_buf   = device_crt_start,
        .clientcert_bytes = (unsigned)(device_crt_end - device_crt_start),
        .clientkey_buf    = device_key_start,
        .clientkey_bytes  = (unsigned)(device_key_end - device_key_start),
        .timeout_ms       = 15000,
        .non_block        = false,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "esp_tls_init() thất bại");
        return NULL;
    }

    if (esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, tls) != 1) {
        ESP_LOGE(TAG, "TLS kết nối tới %s:%d thất bại", host, port);
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    /* Đặt SO_RCVTIMEO = 1s để coreMQTT ProcessLoop không bị block vô hạn */
    int sock = -1;
    if (esp_tls_get_conn_sockfd(tls, &sock) == ESP_OK && sock >= 0) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ESP_LOGI(TAG, "TLS kết nối thành công tới %s:%d", host, port);
    return tls;
}

static void tls_disconnect(esp_tls_t **ptls)
{
    if (ptls && *ptls) {
        esp_tls_conn_destroy(*ptls);
        *ptls = NULL;
        ESP_LOGI(TAG, "TLS đã ngắt kết nối");
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Publish OTA status
 * ══════════════════════════════════════════════════════════════════════════════ */

static void publish_ota_status(const char *status,
                                int         progress,
                                const char *error_msg)
{
    char payload[320];
    int len = snprintf(payload, sizeof(payload),
                       "{\"device_id\":\"%s\",\"status\":\"%s\","
                       "\"progress\":%d,\"version\":\"%s\",\"error\":\"%s\"}",
                       AWS_DEVICE_ID, status, progress,
                       s_ota_version[0] ? s_ota_version : "unknown",
                       error_msg        ? error_msg      : "");
    if (len <= 0 || len >= (int)sizeof(payload)) return;

    MQTTPublishInfo_t pub = {
        .qos             = MQTTQoS0,
        .retain          = false,
        .pTopicName      = TOPIC_OTA_STATUS,
        .topicNameLength = (uint16_t)strlen(TOPIC_OTA_STATUS),
        .pPayload        = payload,
        .payloadLength   = (size_t)len,
    };
    MQTT_Publish(&s_mqtt_ctx, &pub, 0U);
    MQTT_ProcessLoop(&s_mqtt_ctx);  /* flush send buffer */

    ESP_LOGI(TAG, "OTA status published: %s (progress=%d%%)", status, progress);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * MQTT event callback
 * ══════════════════════════════════════════════════════════════════════════════ */

static void event_callback(MQTTContext_t          *pCtx,
                            MQTTPacketInfo_t       *pPacket,
                            MQTTDeserializedInfo_t *pDeserInfo)
{
    (void)pCtx;

    if ((pPacket->type & 0xF0U) != MQTT_PACKET_TYPE_PUBLISH) return;

    MQTTPublishInfo_t *pPub = pDeserInfo->pPublishInfo;
    ESP_LOGI(TAG, "MQTT RX  topic=%.*s",
             (int)pPub->topicNameLength, pPub->pTopicName);

    /* Chỉ xử lý topic OTA command */
    if (pPub->topicNameLength != (uint16_t)strlen(TOPIC_OTA_CMD) ||
        memcmp(pPub->pTopicName, TOPIC_OTA_CMD, pPub->topicNameLength) != 0) {
        return;
    }

    if (s_ota_pending) {
        ESP_LOGW(TAG, "OTA đang chờ xử lý, bỏ qua lệnh trùng lặp");
        return;
    }

    /* Parse JSON fields from payload (not null-terminated, use length-safe scan) */
    const char *payload = (const char *)pPub->pPayload;
    size_t      pay_len = pPub->payloadLength;

    char url_buf[OTA_URL_MAX_LEN] = {0};
    char ver_buf[48]              = {0};

    if (!json_get_string_field(payload, pay_len, "url", url_buf, sizeof(url_buf))
        || url_buf[0] == '\0') {
        ESP_LOGE(TAG, "OTA command thiếu trường 'url'");
        return;
    }

    if (!json_get_string_field(payload, pay_len, "version", ver_buf, sizeof(ver_buf))) {
        strncpy(ver_buf, "unknown", sizeof(ver_buf) - 1);
    }

    /* Parse MD5 (advisory — Secure Boot signature là bảo đảm chính) */
    char md5_buf[OTA_MD5_LEN] = {0};
    if (!json_get_string_field(payload, pay_len, "md5", md5_buf, sizeof(md5_buf))) {
        md5_buf[0] = '\0'; /* MD5 không có trong payload — bỏ qua kiểm tra */
    }

    strncpy(s_ota_url, url_buf, OTA_URL_MAX_LEN - 1);
    s_ota_url[OTA_URL_MAX_LEN - 1] = '\0';

    strncpy(s_ota_version, ver_buf, sizeof(s_ota_version) - 1);
    s_ota_version[sizeof(s_ota_version) - 1] = '\0';

    strncpy(s_ota_md5, md5_buf, OTA_MD5_LEN - 1);
    s_ota_md5[OTA_MD5_LEN - 1] = '\0';

    s_ota_pending = true;

    ESP_LOGI(TAG, "OTA command nhận được: version=%s  md5=%s  url(60)=%.60s...",
             s_ota_version, s_ota_md5[0] ? s_ota_md5 : "(không có)", s_ota_url);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Xác minh MD5 của partition OTA sau khi download
 * ══════════════════════════════════════════════════════════════════════════════ */

static bool verify_partition_md5(const esp_partition_t *part,
                                  size_t                 img_size,
                                  const char            *expected_md5)
{
    if (!expected_md5 || expected_md5[0] == '\0') {
        ESP_LOGW(TAG, "MD5 not provided in OTA command - skipping MD5 check");
        return true; /* Secure Boot signature still verified by esp_https_ota */
    }

    md5_context_t ctx;
    esp_rom_md5_init(&ctx);

    static uint8_t chunk_buf[OTA_MD5_READ_CHUNK]; /* static: avoid stack overflow */
    size_t offset    = 0;
    size_t remaining = img_size;

    while (remaining > 0) {
        size_t to_read = (remaining < OTA_MD5_READ_CHUNK) ? remaining : OTA_MD5_READ_CHUNK;
        if (esp_partition_read(part, offset, chunk_buf, to_read) != ESP_OK) {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %zu", offset);
            return false;
        }
        esp_rom_md5_update(&ctx, chunk_buf, to_read);
        offset    += to_read;
        remaining -= to_read;
    }

    uint8_t digest[16];
    esp_rom_md5_final(digest, &ctx);

    char computed[OTA_MD5_LEN];
    for (int i = 0; i < 16; i++) {
        snprintf(computed + i * 2, 3, "%02x", (unsigned)digest[i]);
    }
    computed[32] = '\0';

    bool match = (strcmp(computed, expected_md5) == 0);
    if (match) {
        ESP_LOGI(TAG, "MD5 OK: %s", computed);
    } else {
        ESP_LOGE(TAG, "MD5 MISMATCH! expected=%s actual=%s", expected_md5, computed);
    }
    return match;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Thực hiện download + flash qua HTTPS OTA (advanced API)
 * Bảo mật:
 *   1. Xác minh MD5 của dữ liệu đã flash trước khi commit partition
 *   2. esp_https_ota_finish() kiểm tra Secure Boot signature tự động
 *      (khi CONFIG_SECURE_BOOT=y)
 *   3. Nếu bất kỳ bước nào thất bại, gọi abort() — thiết bị giữ firmware cũ
 * ══════════════════════════════════════════════════════════════════════════════ */

static esp_err_t do_https_ota(void)
{
    ESP_LOGI(TAG, "Bắt đầu HTTPS OTA (Secure Boot) — version: %s", s_ota_version);

    /* Lưu lại partition sẽ được ghi trước khi bắt đầu download */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "Không tìm thấy OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA partition: %s  offset=0x%08" PRIx32 "  size=0x%08" PRIx32,
             update_partition->label,
             update_partition->address,
             update_partition->size);

    esp_http_client_config_t http_cfg = {
        .url               = s_ota_url,
        .cert_pem          = (const char *)aws_root_ca_start,
        .timeout_ms        = 60000,
        .keep_alive_enable = false,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK || !ota_handle) {
        ESP_LOGE(TAG, "esp_https_ota_begin thất bại: %s", esp_err_to_name(err));
        return err;
    }

    /* ── Download loop ──────────────────────────────────────────── */
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
            ESP_LOGD(TAG, "OTA download: %d bytes", bytes_read);
            continue;
        }
        break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download thất bại: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        return err;
    }

    size_t img_size = (size_t)esp_https_ota_get_image_len_read(ota_handle);
    ESP_LOGI(TAG, "Download hoàn thành: %zu bytes", img_size);

    /* ── Xác minh MD5 trước khi commit ───────────────────────────── */
    if (!verify_partition_md5(update_partition, img_size, s_ota_md5)) {
        ESP_LOGE(TAG, "MD5 không hợp lệ — huỷ OTA, giữ firmware cũ");
        esp_https_ota_abort(ota_handle);
        return ESP_ERR_INVALID_CRC;
    }

    /* ── Commit partition (esp_https_ota_finish kiểm tra Secure Boot sig) ── */
    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA commit thành công — Secure Boot signature hợp lệ");
    } else {
        /* Bao gồm ESP_ERR_OTA_VALIDATE_FAILED nếu Secure Boot signature sai */
        ESP_LOGE(TAG, "OTA finish thất bại: %s", esp_err_to_name(err));
    }
    return err;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Khởi tạo MQTT context (tái sử dụng giữa connect và OTA reconnect)
 * ══════════════════════════════════════════════════════════════════════════════ */

static MQTTStatus_t mqtt_init_and_connect(
        NetworkContext_t        *pNetCtx,
        uint8_t                 *mqtt_buf,
        size_t                   mqtt_buf_sz,
        MQTTPubAckInfo_t        *out_acks,
        MQTTPubAckInfo_t        *in_acks,
        const MQTTConnectInfo_t *conn_info)
{
    TransportInterface_t transport = {
        .recv            = tls_transport_recv,
        .send            = tls_transport_send,
        .pNetworkContext = pNetCtx,
        .writev          = NULL,
    };

    MQTTFixedBuffer_t fixed_buf = {
        .pBuffer = mqtt_buf,
        .size    = mqtt_buf_sz,
    };

    MQTTStatus_t st = MQTT_Init(&s_mqtt_ctx, &transport, get_time_ms,
                                 event_callback, &fixed_buf);
    if (st != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_Init thất bại: %d", (int)st);
        return st;
    }

    memset(out_acks, 0, PENDING_ACK_COUNT * sizeof(MQTTPubAckInfo_t));
    memset(in_acks,  0, PENDING_ACK_COUNT * sizeof(MQTTPubAckInfo_t));
    MQTT_InitStatefulQoS(&s_mqtt_ctx, out_acks, PENDING_ACK_COUNT,
                          in_acks, PENDING_ACK_COUNT);

    bool session_present = false;
    st = MQTT_Connect(&s_mqtt_ctx, conn_info, NULL,
                      MQTT_CONNECT_TIMEOUT_MS, &session_present);
    if (st != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_Connect thất bại: %d", (int)st);
    } else {
        ESP_LOGI(TAG, "MQTT connected  sessionPresent=%d", (int)session_present);
    }
    return st;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Main FreeRTOS task
 * ══════════════════════════════════════════════════════════════════════════════ */

void aws_mqtt_ota_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "AWS MQTT OTA task bắt đầu");

    /* ── Chờ Wi-Fi IP ────────────────────────────────────────────────────── */
    {
        uint32_t elapsed = 0;
        bool     got_ip  = false;
        while (elapsed < WIFI_WAIT_TIMEOUT_MS) {
            esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (ni) {
                esp_netif_ip_info_t ip;
                if (esp_netif_get_ip_info(ni, &ip) == ESP_OK && ip.ip.addr != 0) {
                    ESP_LOGI(TAG, "Đã có IP: " IPSTR, IP2STR(&ip.ip));
                    got_ip = true;
                    vTaskDelay(pdMS_TO_TICKS(2000U));
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_WAIT_MS));
            elapsed += WIFI_WAIT_MS;
            ESP_LOGI(TAG, "Chờ IP... (%lu/%lu ms)",
                     (unsigned long)elapsed, (unsigned long)WIFI_WAIT_TIMEOUT_MS);
        }
        if (!got_ip) {
            ESP_LOGE(TAG, "Timeout chờ IP — task kết thúc");
            vTaskDelete(NULL);
            return;
        }
    }

    /* Buffers khai báo static để không chiếm stack */
    static uint8_t          mqtt_buf[MQTT_NETWORK_BUF_SIZE];
    static MQTTPubAckInfo_t out_acks[PENDING_ACK_COUNT];
    static MQTTPubAckInfo_t in_acks[PENDING_ACK_COUNT];

    MQTTConnectInfo_t conn_info = {
        .cleanSession           = true,
        .pClientIdentifier      = AWS_DEVICE_ID,
        .clientIdentifierLength = (uint16_t)strlen(AWS_DEVICE_ID),
        .keepAliveSeconds       = MQTT_KEEP_ALIVE_SEC,
        .pUserName              = NULL,
        .userNameLength         = 0,
        .pPassword              = NULL,
        .passwordLength         = 0,
    };

    uint32_t attempt = 0;

    while (1) {
        attempt++;
        ESP_LOGI(TAG, "=== AWS IoT kết nối lần #%lu ===", (unsigned long)attempt);

        /* ── Kiểm tra Wi-Fi trước mỗi lần kết nối ─────────────────────── */
        {
            bool wifi_ok = false;
            esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (ni) {
                esp_netif_ip_info_t ip;
                if (esp_netif_get_ip_info(ni, &ip) == ESP_OK && ip.ip.addr != 0)
                    wifi_ok = true;
            }
            if (!wifi_ok) {
                ESP_LOGW(TAG, "Wi-Fi chưa sẵn sàng, thử lại sau %d ms",
                         MQTT_RECONNECT_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
                continue;
            }
        }

        /* ── TLS kết nối tới AWS IoT Core ──────────────────────────────── */
        NetworkContext_t net_ctx = { .tls = NULL };
        net_ctx.tls = tls_connect(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
        if (!net_ctx.tls) {
            vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
            continue;
        }

        /* ── MQTT Init + Connect ────────────────────────────────────────── */
        if (mqtt_init_and_connect(&net_ctx, mqtt_buf, sizeof(mqtt_buf),
                                   out_acks, in_acks, &conn_info) != MQTTSuccess) {
            tls_disconnect(&net_ctx.tls);
            vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
            continue;
        }

        /* ── Subscribe OTA command topic ───────────────────────────────── */
        MQTTSubscribeInfo_t sub = {
            .qos               = MQTTQoS1,
            .pTopicFilter      = TOPIC_OTA_CMD,
            .topicFilterLength = (uint16_t)strlen(TOPIC_OTA_CMD),
        };
        uint16_t sub_pkt = MQTT_GetPacketId(&s_mqtt_ctx);
        if (MQTT_Subscribe(&s_mqtt_ctx, &sub, 1U, sub_pkt) != MQTTSuccess)
            ESP_LOGE(TAG, "MQTT_Subscribe thất bại");
        MQTT_ProcessLoop(&s_mqtt_ctx);
        ESP_LOGI(TAG, "Đã subscribe: %s", TOPIC_OTA_CMD);

        /* ── ProcessLoop — chờ OTA command ────────────────────────────── */
        s_ota_pending = false;
        MQTTStatus_t loop_status = MQTTSuccess;

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(MQTT_PROCESS_LOOP_MS));
            loop_status = MQTT_ProcessLoop(&s_mqtt_ctx);
            if (loop_status != MQTTSuccess) {
                ESP_LOGE(TAG, "MQTT_ProcessLoop lỗi: %d — kết nối lại",
                         (int)loop_status);
                break;
            }
            if (s_ota_pending) {
                /* Publish "downloading" trước khi ngắt kết nối */
                publish_ota_status("downloading", 0, NULL);
                break;
            }
        }

        /* ── Ngắt MQTT + TLS ────────────────────────────────────────────── */
        MQTT_Disconnect(&s_mqtt_ctx);
        tls_disconnect(&net_ctx.tls);

        /* ── Thực hiện OTA nếu có lệnh pending ─────────────────────────── */
        if (s_ota_pending) {
            s_ota_pending = false;

            esp_err_t ota_err = do_https_ota();

            /* Kết nối lại để publish kết quả cuối */
            vTaskDelay(pdMS_TO_TICKS(3000U));
            net_ctx.tls = tls_connect(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
            if (net_ctx.tls &&
                mqtt_init_and_connect(&net_ctx, mqtt_buf, sizeof(mqtt_buf),
                                       out_acks, in_acks, &conn_info) == MQTTSuccess) {
                if (ota_err == ESP_OK) {
                    publish_ota_status("success", 100, NULL);
                } else {
                    publish_ota_status("failed", 0, esp_err_to_name(ota_err));
                }
                MQTT_Disconnect(&s_mqtt_ctx);
            }
            tls_disconnect(&net_ctx.tls);

            if (ota_err == ESP_OK) {
                ESP_LOGI(TAG, "OTA hoàn thành — khởi động lại sau 2s...");
                vTaskDelay(pdMS_TO_TICKS(2000U));
                esp_restart();
            }
            /* OTA thất bại: tiếp tục vòng lặp ngoài để kết nối lại */
        }

        ESP_LOGW(TAG, "Thử kết nối lại sau %d ms...", MQTT_RECONNECT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
    }
}
