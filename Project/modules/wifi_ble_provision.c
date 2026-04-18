#include "wifi_ble_provision.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define WIFI_NVS_NAMESPACE            "wifi_cfg"
#define WIFI_NVS_KEY_SSID             "ssid"
#define WIFI_NVS_KEY_PASS             "pass"
#define WIFI_MAX_RETRY                8
#define WIFI_STATUS_MAX_LEN           64

#define CLEAR_BUTTON_GPIO             GPIO_NUM_0
#define CLEAR_BUTTON_ACTIVE_LEVEL     0
#define CLEAR_BUTTON_HOLD_TIME_MS     3000
#define CLEAR_BUTTON_POLL_MS          50

#define PROV_DEVICE_NAME              "ESP32-WIFI-PROV"

static const ble_uuid128_t g_service_uuid = BLE_UUID128_INIT(
    0xf0, 0xde, 0xbc, 0x9a,
    0x78, 0x56,
    0x34, 0x12,
    0x78, 0x56,
    0x34, 0x12, 0x78, 0x56, 0x34, 0x12
);

static const ble_uuid128_t g_ssid_char_uuid = BLE_UUID128_INIT(
    0xf1, 0xde, 0xbc, 0x9a,
    0x78, 0x56,
    0x34, 0x12,
    0x78, 0x56,
    0x34, 0x12, 0x78, 0x56, 0x34, 0x12
);

static const ble_uuid128_t g_pass_char_uuid = BLE_UUID128_INIT(
    0xf2, 0xde, 0xbc, 0x9a,
    0x78, 0x56,
    0x34, 0x12,
    0x78, 0x56,
    0x34, 0x12, 0x78, 0x56, 0x34, 0x12
);

static const ble_uuid128_t g_status_char_uuid = BLE_UUID128_INIT(
    0xf3, 0xde, 0xbc, 0x9a,
    0x78, 0x56,
    0x34, 0x12,
    0x78, 0x56,
    0x34, 0x12, 0x78, 0x56, 0x34, 0x12
);

static const char *TAG = "WIFI_BLE_PROV";

static EventGroupHandle_t g_wifi_event_group;
static int g_wifi_retry;
static bool g_wifi_initialized;
static uint8_t g_ble_addr_type;

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_ssid_val_handle;
static uint16_t g_pass_val_handle;
static uint16_t g_status_val_handle;

static char g_received_ssid[33];
static char g_received_pass[65];
static bool g_has_ssid;
static bool g_has_pass;
static char g_status_payload[WIFI_STATUS_MAX_LEN];

static esp_err_t provisioning_try_connect(void);
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);
static esp_err_t wifi_ensure_initialized(void);

/* Forward declaration matching NimBLE internal store – no public header exposes this. */
void ble_store_config_init(void);

static void provisioning_notify_status(const char *status)
{
    if (status == NULL) {
        return;
    }

    strncpy(g_status_payload, status, sizeof(g_status_payload) - 1);
    g_status_payload[sizeof(g_status_payload) - 1] = '\0';

    ESP_LOGI(TAG, "Status: %s", g_status_payload);

    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_status_val_handle == 0) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(g_status_payload, strlen(g_status_payload));
    if (om == NULL) {
        ESP_LOGW(TAG, "Cannot allocate notify buffer");
        return;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_status_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify failed rc=%d", rc);
    }
}

static esp_err_t wifi_store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs_handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG, "nvs_open failed");

    esp_err_t err = nvs_set_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, WIFI_NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t wifi_load_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t req_ssid = ssid_len;
    size_t req_pass = pass_len;
    err = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid_out, &req_ssid);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASS, pass_out, &req_pass);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t nvs_handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG, "nvs_open failed");

    esp_err_t err = nvs_erase_key(nvs_handle, WIFI_NVS_KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        esp_err_t pass_err = nvs_erase_key(nvs_handle, WIFI_NVS_KEY_PASS);
        if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) {
            err = pass_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_wifi_retry < WIFI_MAX_RETRY) {
            g_wifi_retry++;
            esp_wifi_connect();
            provisioning_notify_status("WIFI_RETRYING");
        } else {
            provisioning_notify_status("WIFI_FAILED");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_wifi_event_group, BIT0);
        provisioning_notify_status("WIFI_CONNECTED");
    }
}

static esp_err_t wifi_init_sta(void)
{
    if (g_wifi_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
        TAG,
        "register WIFI_EVENT failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
        TAG,
        "register IP_EVENT failed"
    );

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    g_wifi_initialized = true;

    return ESP_OK;
}

static esp_err_t wifi_ensure_initialized(void)
{
    return wifi_init_sta();
}

static esp_err_t provisioning_try_connect(void)
{
    if (!g_has_ssid || !g_has_pass) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(wifi_ensure_initialized(), TAG, "wifi init failed");

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, g_received_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, g_received_pass, sizeof(wifi_config.sta.password) - 1);

    g_wifi_retry = 0;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "esp_wifi_connect failed");

    provisioning_notify_status("WIFI_CONNECTING");
    return ESP_OK;
}

static int provisioning_gatt_access(uint16_t conn_handle,
                                    uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt,
                                    void *arg)
{
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);

        if (attr_handle == g_ssid_val_handle) {
            if (len <= 0 || len > 32) {
                provisioning_notify_status("SSID_INVALID");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            memset(g_received_ssid, 0, sizeof(g_received_ssid));
            os_mbuf_copydata(ctxt->om, 0, len, g_received_ssid);
            g_received_ssid[len] = '\0';
            g_has_ssid = true;
            provisioning_notify_status("SSID_RECEIVED");
        } else if (attr_handle == g_pass_val_handle) {
            if (len <= 0 || len > 64) {
                provisioning_notify_status("PASS_INVALID");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            memset(g_received_pass, 0, sizeof(g_received_pass));
            os_mbuf_copydata(ctxt->om, 0, len, g_received_pass);
            g_received_pass[len] = '\0';
            g_has_pass = true;
            provisioning_notify_status("PASS_RECEIVED");
        }

        if (g_has_ssid && g_has_pass) {
            esp_err_t err = wifi_store_credentials(g_received_ssid, g_received_pass);
            if (err != ESP_OK) {
                provisioning_notify_status("NVS_SAVE_FAILED");
                return BLE_ATT_ERR_UNLIKELY;
            }

            provisioning_notify_status("NVS_SAVED");

            err = provisioning_try_connect();
            if (err != ESP_OK) {
                provisioning_notify_status("WIFI_START_FAILED");
                return BLE_ATT_ERR_UNLIKELY;
            }
        }

        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == g_status_val_handle) {
        int rc = os_mbuf_append(ctxt->om, g_status_payload, strlen(g_status_payload));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &g_ssid_char_uuid.u,
                .access_cb = provisioning_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &g_ssid_val_handle,
            },
            {
                .uuid = &g_pass_char_uuid.u,
                .access_cb = provisioning_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &g_pass_val_handle,
            },
            {
                .uuid = &g_status_char_uuid.u,
                .access_cb = provisioning_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &g_status_val_handle,
            },
            {0}
        },
    },
    {0}
};

static void ble_advertise(void)
{
    if (ble_gap_adv_active()) {
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)PROV_DEVICE_NAME;
    fields.name_len = strlen(PROV_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    rsp_fields.uuids128 = (ble_uuid128_t *)&g_service_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_rsp_set_fields failed rc=%d", rc);
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        g_ble_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        ble_gap_event_handler,
        NULL
    );
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
    }
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            provisioning_notify_status("BLE_CONNECTED");
        } else {
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_status_val_handle) {
            provisioning_notify_status("READY");
        }
        break;

    default:
        break;
    }

    return 0;
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d; re-advertising on next sync", reason);
}

static void ble_on_sync(void)
{
    uint8_t addr_val[6] = {0};
    int rc = ble_hs_id_infer_auto(0, &g_ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_copy_addr(g_ble_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_copy_addr failed rc=%d", rc);
        return;
    }

    ESP_LOGI(
        TAG,
        "BLE addr: %02x:%02x:%02x:%02x:%02x:%02x",
        addr_val[5],
        addr_val[4],
        addr_val[3],
        addr_val[2],
        addr_val[1],
        addr_val[0]
    );

    ble_advertise();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set(PROV_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(gatt_services);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_services);
    if (rc != 0) {
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb   = ble_on_reset;
    ble_hs_cfg.sync_cb    = ble_on_sync;

    /* Disable pairing/bonding – provisioning app does not need encryption.
     * This prevents Windows from using stale bond keys after a re-flash. */
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 0;

    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

static void clear_button_task(void *arg)
{
    (void)arg;

    TickType_t press_start = 0;

    while (1) {
        int level = gpio_get_level(CLEAR_BUTTON_GPIO);
        bool pressed = (level == CLEAR_BUTTON_ACTIVE_LEVEL);

        if (pressed && press_start == 0) {
            press_start = xTaskGetTickCount();
        }

        if (!pressed) {
            press_start = 0;
        }

        if (pressed && press_start != 0) {
            TickType_t held_ticks = xTaskGetTickCount() - press_start;
            if (held_ticks >= pdMS_TO_TICKS(CLEAR_BUTTON_HOLD_TIME_MS)) {
                provisioning_notify_status("CONFIG_CLEARED");
                if (wifi_clear_credentials() != ESP_OK) {
                    provisioning_notify_status("CLEAR_FAILED");
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CLEAR_BUTTON_POLL_MS));
    }
}

static esp_err_t clear_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CLEAR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    BaseType_t task_ok = xTaskCreate(clear_button_task, "clear_btn", 2048, NULL, 5, NULL);
    return (task_ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_ble_provisioning_init(void)
{
    g_wifi_event_group = xEventGroupCreate();
    if (g_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    ESP_RETURN_ON_ERROR(ble_init(), TAG, "ble_init failed");
    ESP_RETURN_ON_ERROR(clear_button_init(), TAG, "clear_button_init failed");

    memset(g_status_payload, 0, sizeof(g_status_payload));
    strcpy(g_status_payload, "READY");

    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};
    if (wifi_load_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) == ESP_OK &&
        strlen(saved_ssid) > 0 && strlen(saved_pass) > 0) {
        strncpy(g_received_ssid, saved_ssid, sizeof(g_received_ssid) - 1);
        strncpy(g_received_pass, saved_pass, sizeof(g_received_pass) - 1);
        g_has_ssid = true;
        g_has_pass = true;

        ESP_LOGI(TAG, "Found saved credentials, connecting WiFi");
        ESP_RETURN_ON_ERROR(provisioning_try_connect(), TAG, "provisioning_try_connect failed");
    } else {
        ESP_LOGI(TAG, "No saved credentials, BLE-only waiting mode");
    }

    return ESP_OK;
}
