
#include "mqtt_client_task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "core_mqtt.h"

/* ── Configuration ──────────────────────────────────────────────────────── */
#define MQTT_BROKER_HOST        "broker.hivemq.com"
#define MQTT_BROKER_PORT        1883

#define MQTT_CLIENT_ID          "esp32-group2-001"
#define MQTT_KEEP_ALIVE_SEC     60

#define TOPIC_SUBSCRIBE         "esp32/from_mqttx"
#define TOPIC_PUBLISH           "esp32/to_mqttx"

#define MQTT_NETWORK_BUF_SIZE   2048U
#define TCP_TIMEOUT_SEC         10
#define WIFI_WAIT_MS            1000
#define WIFI_WAIT_TIMEOUT_MS    60000
#define DNS_RETRY_COUNT         5
#define DNS_RETRY_DELAY_MS      2000
#define MQTT_CONNECT_TIMEOUT_MS 5000U
#define MQTT_PROCESS_LOOP_MS    1000U
#define MQTT_RECONNECT_DELAY_MS 10000U /* delay between reconnect attempts */

/* Publish interval — send a test payload every N process-loop iterations */
#define PUBLISH_INTERVAL_LOOPS  5

static const char *TAG = "MQTT_CLIENT";

// ── Pending echo (deferred from callback → main loop) ────────────────────
// MQTT_Publish must NOT be called from inside event_callback because the
// callback runs inside MQTT_ProcessLoop — re-entrant usage corrupts the
// keep-alive timer and the packet-state machine.  Instead we store the
// payload here and publish it right after MQTT_ProcessLoop returns.
static volatile bool s_echo_pending     = false;
static char          s_echo_payload[256];
static size_t        s_echo_payload_len = 0U;

// NetworkContext (required by coreMQTT transport interface) 
struct NetworkContext
{
    int tcpSocket;
};

//Transport callbacks 
static int32_t transport_recv( NetworkContext_t *pCtx,
                               void             *pBuffer,
                               size_t            bytesToRecv )
{
    int32_t received = (int32_t) recv( pCtx->tcpSocket, pBuffer,
                                       bytesToRecv, 0 );
    if( received == 0 )
    {
        // Peer closed connection.
        return -1;
    }
    if( received < 0 )
    {
        if( errno == EAGAIN || errno == EWOULDBLOCK )
        {
            // Timeout — return 0 so coreMQTT retries
            return 0;
        }
        ESP_LOGE( TAG, "recv() error: %d", errno );
        return -1;
    }
    return received;
}

static int32_t transport_send( NetworkContext_t *pCtx,
                               const void       *pBuffer,
                               size_t            bytesToSend )
{
    int32_t sent = (int32_t) send( pCtx->tcpSocket, pBuffer,
                                   bytesToSend, 0 );
    if( sent < 0 )
    {
        ESP_LOGE( TAG, "send() error: %d", errno );
    }
    return sent;
}

//Clock helper (milliseconds) required by MQTT_Init 

static uint32_t get_time_ms( void )
{
    return (uint32_t)( esp_timer_get_time() / 1000ULL );
}

// TCP helpers 

static int tcp_connect( NetworkContext_t *pCtx,
                        const char       *host,
                        uint16_t          port )
{
    struct addrinfo hints;
    memset( &hints, 0, sizeof( hints ) );
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf( port_str, sizeof( port_str ), "%u", (unsigned) port );

    struct addrinfo *res = NULL;
    int err = 0;
    for( int attempt = 1; attempt <= DNS_RETRY_COUNT; attempt++ )
    {
        ESP_LOGI( TAG, "DNS lookup: %s (attempt %d/%d)...",
                  host, attempt, DNS_RETRY_COUNT );
        res = NULL;
        err = getaddrinfo( host, port_str, &hints, &res );
        if( err == 0 && res != NULL )
        {
            break;
        }
        ESP_LOGW( TAG, "getaddrinfo failed: %d — retry in %d ms",
                  err, DNS_RETRY_DELAY_MS );
        if( res != NULL ) { freeaddrinfo( res ); res = NULL; }
        vTaskDelay( pdMS_TO_TICKS( DNS_RETRY_DELAY_MS ) );
    }
    if( err != 0 || res == NULL )
    {
        ESP_LOGE( TAG, "DNS failed after %d attempts", DNS_RETRY_COUNT );
        return -1;
    }

    char ip_str[16];
    struct sockaddr_in *addr4 = (struct sockaddr_in *) res->ai_addr;
    inet_ntop( AF_INET, &addr4->sin_addr, ip_str, sizeof( ip_str ) );
    ESP_LOGI( TAG, "Resolved %s -> %s", host, ip_str );

    int sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if( sock < 0 )
    {
        ESP_LOGE( TAG, "socket() failed: %d", errno );
        freeaddrinfo( res );
        return -1;
    }

    /* SO_RCVTIMEO — keep short (1 s) so MQTT_Connect() can retry within its
     * own deadline (MQTT_CONNECT_TIMEOUT_MS).  A value ≥ MQTT_CONNECT_TIMEOUT_MS
     * would cause MQTT_Connect to time-out on the first blocked recv(). */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );

    /* Non-blocking connect + select() so we get a hard TCP_TIMEOUT_SEC timeout
     * and proper EHOSTUNREACH detection (port blocked by firewall). */
    int fl = fcntl( sock, F_GETFL, 0 );
    fcntl( sock, F_SETFL, fl | O_NONBLOCK );

    int cret = connect( sock, res->ai_addr, res->ai_addrlen );
    freeaddrinfo( res );

    if( cret != 0 && errno != EINPROGRESS )
    {
        int saved = errno;
        close( sock );
        if( saved == EHOSTUNREACH || saved == ENETUNREACH )
        {
            ESP_LOGE( TAG, "connect() errno=%d (EHOSTUNREACH): port %u is likely "
                      "BLOCKED by your router/firewall.\n"
                      ">>> Switch to a mobile hotspot and retry. <<<",
                      saved, (unsigned) port );
        }
        else
        {
            ESP_LOGE( TAG, "connect() to %s:%u failed immediately: errno=%d",
                      host, (unsigned) port, saved );
        }
        return -1;
    }

    if( cret != 0 )   /* EINPROGRESS — wait for socket writable */
    {
        fd_set wfds, efds;
        FD_ZERO( &wfds );  FD_SET( sock, &wfds );
        FD_ZERO( &efds );  FD_SET( sock, &efds );
        struct timeval sel_tv = { .tv_sec = TCP_TIMEOUT_SEC, .tv_usec = 0 };
        int sel = select( sock + 1, NULL, &wfds, &efds, &sel_tv );
        if( sel <= 0 )
        {
            ESP_LOGE( TAG, "connect() to %s:%u timed out after %ds",
                      host, (unsigned) port, TCP_TIMEOUT_SEC );
            close( sock );
            return -1;
        }
        int so_err = 0;
        socklen_t so_len = sizeof( so_err );
        getsockopt( sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len );
        if( so_err != 0 )
        {
            close( sock );
            if( so_err == EHOSTUNREACH || so_err == ENETUNREACH )
            {
                ESP_LOGE( TAG, "connect() errno=%d (EHOSTUNREACH): port %u is likely "
                          "BLOCKED by your router/firewall.\n"
                          ">>> Switch to a mobile hotspot and retry. <<<",
                          so_err, (unsigned) port );
            }
            else
            {
                ESP_LOGE( TAG, "connect() to %s:%u failed: errno=%d",
                          host, (unsigned) port, so_err );
            }
            return -1;
        }
    }

    /* Restore blocking mode for subsequent send/recv */
    fl = fcntl( sock, F_GETFL, 0 );
    fcntl( sock, F_SETFL, fl & ~O_NONBLOCK );

    ESP_LOGI( TAG, "TCP connected to %s:%u (sock=%d)", host, (unsigned) port, sock );
    pCtx->tcpSocket = sock;
    return 0;
}

static void tcp_disconnect( NetworkContext_t *pCtx )
{
    if( pCtx->tcpSocket >= 0 )
    {
        shutdown( pCtx->tcpSocket, SHUT_RDWR );
        close( pCtx->tcpSocket );
        pCtx->tcpSocket = -1;
        ESP_LOGI( TAG, "TCP socket closed" );
    }
}

/* ── JSON string escaper ────────────────────────────────────────────────── */
/* Escapes src into dst as a JSON string value (without surrounding quotes).
 * Returns number of bytes written (not including NUL), or -1 if dst too small.*/
static int json_escape( char *dst, size_t dst_size,
                        const char *src, size_t src_len )
{
    size_t di = 0;
    for( size_t si = 0; si < src_len; si++ )
    {
        char c   = src[ si ];
        char esc = 0;
        if     ( c == '"'  ) { esc = '"';  }
        else if( c == '\\' ) { esc = '\\'; }
        else if( c == '\n' ) { esc = 'n';  }
        else if( c == '\r' ) { esc = 'r';  }
        else if( c == '\t' ) { esc = 't';  }

        if( esc != 0 )
        {
            if( di + 2 >= dst_size ) { return -1; }
            dst[ di++ ] = '\\';
            dst[ di++ ] = esc;
        }
        else
        {
            if( di + 1 >= dst_size ) { return -1; }
            dst[ di++ ] = c;
        }
    }
    dst[ di ] = '\0';
    return (int) di;
}

/* ── Bước 8: eventCallback ───────────────────────────────────────────────── */

static void event_callback( MQTTContext_t          *pCtx,
                             MQTTPacketInfo_t       *pPacket,
                             MQTTDeserializedInfo_t *pDeserInfo )
{
    if( ( pPacket->type & 0xF0U ) == MQTT_PACKET_TYPE_PUBLISH )
    {
        MQTTPublishInfo_t *pPub = pDeserInfo->pPublishInfo;

        ESP_LOGI( TAG, "=== MQTT INCOMING ===" );
        ESP_LOGI( TAG, "Topic  : %.*s",
                  (int) pPub->topicNameLength, pPub->pTopicName );
        ESP_LOGI( TAG, "Payload: %.*s",
                  (int) pPub->payloadLength, (char *) pPub->pPayload );
        ESP_LOGI( TAG, "QoS    : %d", (int) pPub->qos );

                
        char escaped[ 200 ];
        char resp[ 256 ];
        int resp_len = -1;
        if( json_escape( escaped, sizeof( escaped ),
                         (char *) pPub->pPayload,
                         pPub->payloadLength ) >= 0 )
        {
            resp_len = snprintf( resp, sizeof( resp ),
                                 "{\"echo\":\"%s\",\"from\":\"esp32\"}",
                                 escaped );
        }
        if( resp_len > 0 && resp_len < (int) sizeof( resp ) )
        {
            /* Defer — do NOT call MQTT_Publish here; we are inside
             * MQTT_ProcessLoop which already owns the MQTT context.
             * Re-entrant publish corrupts the keep-alive timer.        */
            memcpy( s_echo_payload, resp, (size_t) resp_len );
            s_echo_payload_len = (size_t) resp_len;
            s_echo_pending     = true;
            ESP_LOGI( TAG, "Echo queued — will publish after ProcessLoop" );
        }
    }
    else if( pPacket->type == MQTT_PACKET_TYPE_PUBACK )
    {
        ESP_LOGI( TAG, "PUBACK received — packetID: %u",
                  (unsigned) pDeserInfo->packetIdentifier );
    }
    else if( ( pPacket->type & 0xF0U ) == MQTT_PACKET_TYPE_SUBACK )
    {
        ESP_LOGI( TAG, "SUBACK received — subscribed OK" );
    }
}

/* ── Main FreeRTOS task ──────────────────────────────────────────────────── */

void mqtt_client_task( void *pvParameters )
{
    (void) pvParameters;

    /* ── Bước 1: Wait for Wi-Fi IP ─────────────────────────────────────── */
    ESP_LOGI( TAG, "MQTT task started — waiting for Wi-Fi IP..." );
    uint32_t elapsed = 0;
    bool got_ip = false;
    while( elapsed < WIFI_WAIT_TIMEOUT_MS )
    {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey( "WIFI_STA_DEF" );
        if( netif != NULL )
        {
            esp_netif_ip_info_t ip_info;
            if( esp_netif_get_ip_info( netif, &ip_info ) == ESP_OK &&
                ip_info.ip.addr != 0 )
            {
                ESP_LOGI( TAG, "Got IP: " IPSTR, IP2STR( &ip_info.ip ) );
                got_ip = true;
                vTaskDelay( pdMS_TO_TICKS( 2000U ) ); /* let routing stabilize */
                break;
            }
        }
        vTaskDelay( pdMS_TO_TICKS( WIFI_WAIT_MS ) );
        elapsed += WIFI_WAIT_MS;
        ESP_LOGI( TAG, "Waiting for IP... (%lu/%lu ms)",
                  (unsigned long) elapsed, (unsigned long) WIFI_WAIT_TIMEOUT_MS );
    }
    if( !got_ip )
    {
        ESP_LOGE( TAG, "Timed out waiting for IP — MQTT task abort" );
        vTaskDelete( NULL );
        return;
    }

    /* ── Outer reconnect loop — runs forever, never calls vTaskDelete ─── */
    static uint8_t mqtt_buffer[ MQTT_NETWORK_BUF_SIZE ];
    uint32_t reconnect_num = 0;

    while( 1 )
    {
        reconnect_num++;
        ESP_LOGI( TAG, "=== MQTT connection attempt #%lu ===",
                  (unsigned long) reconnect_num );

        /* Quick Wi-Fi check before each attempt */
        {
            bool wifi_up = false;
            esp_netif_t *ni = esp_netif_get_handle_from_ifkey( "WIFI_STA_DEF" );
            if( ni )
            {
                esp_netif_ip_info_t ipi;
                if( esp_netif_get_ip_info( ni, &ipi ) == ESP_OK && ipi.ip.addr != 0 )
                    wifi_up = true;
            }
            if( !wifi_up )
            {
                ESP_LOGW( TAG, "Wi-Fi not ready — wait %d ms",
                          MQTT_RECONNECT_DELAY_MS );
                vTaskDelay( pdMS_TO_TICKS( MQTT_RECONNECT_DELAY_MS ) );
                continue;
            }
        }

        /* ── Bước 2 & 3: DNS + TCP ─────────────────────────────────────── */
        NetworkContext_t netCtx = { .tcpSocket = -1 };
        if( tcp_connect( &netCtx, MQTT_BROKER_HOST, MQTT_BROKER_PORT ) != 0 )
        {
            ESP_LOGW( TAG, "TCP connect failed — retry in %d ms",
                      MQTT_RECONNECT_DELAY_MS );
            vTaskDelay( pdMS_TO_TICKS( MQTT_RECONNECT_DELAY_MS ) );
            continue;
        }

        /* ── Bước 3: Transport Interface ───────────────────────────────── */
        TransportInterface_t transport = {
            .recv            = transport_recv,
            .send            = transport_send,
            .pNetworkContext = &netCtx,
            .writev          = NULL,
        };

        /* ── Bước 4: MQTT_Init() ────────────────────────────────────────── */
        MQTTFixedBuffer_t fixed_buf = {
            .pBuffer = mqtt_buffer,
            .size    = sizeof( mqtt_buffer ),
        };

        MQTTContext_t mqtt_ctx;
        MQTTStatus_t status = MQTT_Init( &mqtt_ctx,
                                         &transport,
                                         get_time_ms,
                                         event_callback,
                                         &fixed_buf );
        if( status != MQTTSuccess )
        {
            ESP_LOGE( TAG, "MQTT_Init failed: %d", (int) status );
            tcp_disconnect( &netCtx );
            vTaskDelay( pdMS_TO_TICKS( MQTT_RECONNECT_DELAY_MS ) );
            continue;
        }

        /* ── Enable QoS1/QoS2 stateful tracking ────────────────────────── */
#define MQTT_PENDING_ACK_COUNT  10U
        static MQTTPubAckInfo_t outgoing_acks[ MQTT_PENDING_ACK_COUNT ];
        static MQTTPubAckInfo_t incoming_acks[ MQTT_PENDING_ACK_COUNT ];
        /* CRITICAL: MQTT_InitStatefulQoS only stores the pointer — it does NOT
         * zero the arrays.  On reconnect the arrays may contain stale slot
         * entries from the previous session (packetId != 0), causing addRecord()
         * to think the table is full and making MQTT_Publish fail silently.
         * Always clear them before every MQTT_InitStatefulQoS call.          */
        memset( outgoing_acks, 0, sizeof( outgoing_acks ) );
        memset( incoming_acks, 0, sizeof( incoming_acks ) );
        status = MQTT_InitStatefulQoS( &mqtt_ctx,
                                       outgoing_acks, MQTT_PENDING_ACK_COUNT,
                                       incoming_acks, MQTT_PENDING_ACK_COUNT );
        if( status != MQTTSuccess )
        {
            ESP_LOGE( TAG, "MQTT_InitStatefulQoS failed: %d", (int) status );
            tcp_disconnect( &netCtx );
            vTaskDelay( pdMS_TO_TICKS( MQTT_RECONNECT_DELAY_MS ) );
            continue;
        }
        ESP_LOGI( TAG, "MQTT_Init OK" );

        /* ── Bước 5: MQTT_Connect() ─────────────────────────────────────── */
        MQTTConnectInfo_t connect_info = {
            .cleanSession           = true,
            .pClientIdentifier      = MQTT_CLIENT_ID,
            .clientIdentifierLength = (uint16_t) strlen( MQTT_CLIENT_ID ),
            .keepAliveSeconds       = MQTT_KEEP_ALIVE_SEC,
            .pUserName              = NULL,
            .userNameLength         = 0,
            .pPassword              = NULL,
            .passwordLength         = 0,
        };

        bool session_present = false;
        status = MQTT_Connect( &mqtt_ctx,
                               &connect_info,
                               NULL,
                               MQTT_CONNECT_TIMEOUT_MS,
                               &session_present );
        if( status != MQTTSuccess )
        {
            ESP_LOGE( TAG, "MQTT_Connect failed: %d", (int) status );
            tcp_disconnect( &netCtx );
            vTaskDelay( pdMS_TO_TICKS( MQTT_RECONNECT_DELAY_MS ) );
            continue;
        }
        ESP_LOGI( TAG, "MQTT connected to %s:%d  sessionPresent=%d",
                  MQTT_BROKER_HOST, MQTT_BROKER_PORT, (int) session_present );

        /* ── Bước 6: MQTT_Subscribe() ──────────────────────────────────── */
        MQTTSubscribeInfo_t sub_info = {
            .qos               = MQTTQoS1,
            .pTopicFilter      = TOPIC_SUBSCRIBE,
            .topicFilterLength = (uint16_t) strlen( TOPIC_SUBSCRIBE ),
        };
        uint16_t sub_packet_id = MQTT_GetPacketId( &mqtt_ctx );
        status = MQTT_Subscribe( &mqtt_ctx, &sub_info, 1U, sub_packet_id );
        if( status != MQTTSuccess )
            ESP_LOGE( TAG, "MQTT_Subscribe failed: %d", (int) status );

        MQTT_ProcessLoop( &mqtt_ctx );
        ESP_LOGI( TAG, "Subscribed to: %s", TOPIC_SUBSCRIBE );

        /* ── Bước 7: Publish + ProcessLoop ──────────────────────────────── */
        const char *pub_payload =
            "{\"device\":\"esp32-group2\",\"sensor\":\"temp\",\"value\":28.5}";
        MQTTPublishInfo_t pub_info = {
            .qos             = MQTTQoS1,
            .retain          = false,
            .pTopicName      = TOPIC_PUBLISH,
            .topicNameLength = (uint16_t) strlen( TOPIC_PUBLISH ),
            .pPayload        = pub_payload,
            .payloadLength   = strlen( pub_payload ),
        };

        uint16_t pub_packet_id = MQTT_GetPacketId( &mqtt_ctx );
        if( MQTT_Publish( &mqtt_ctx, &pub_info, pub_packet_id ) == MQTTSuccess )
            ESP_LOGI( TAG, "Published to %s (packetID=%u)",
                      TOPIC_PUBLISH, (unsigned) pub_packet_id );

        /* Inner process loop — exits on error, then outer loop reconnects */
        int loop_count = 0;
        s_echo_pending = false;   /* discard any stale echo from previous session */
        while( 1 )
        {
            vTaskDelay( pdMS_TO_TICKS( MQTT_PROCESS_LOOP_MS ) );
            status = MQTT_ProcessLoop( &mqtt_ctx );
            if( status != MQTTSuccess )
            {
                ESP_LOGE( TAG, "MQTT_ProcessLoop error: %d", (int) status );
                break;
            }

            /* Publish deferred echo (queued by event_callback) */
            if( s_echo_pending )
            {
                s_echo_pending = false;
                MQTTPublishInfo_t echo_pub = {
                    .qos             = MQTTQoS0,   /* QoS0: fire-and-forget, no
                                                    * outgoing_acks slot needed,
                                                    * eliminates stale-state risk */
                    .retain          = false,
                    .pTopicName      = TOPIC_PUBLISH,
                    .topicNameLength = (uint16_t) strlen( TOPIC_PUBLISH ),
                    .pPayload        = s_echo_payload,
                    .payloadLength   = s_echo_payload_len,
                };
                /* packetId must be 0 for QoS0 */
                MQTTStatus_t st = MQTT_Publish( &mqtt_ctx, &echo_pub, 0U );
                if( st == MQTTSuccess )
                    ESP_LOGI( TAG, "Echo published (QoS0) to %s: %.*s",
                              TOPIC_PUBLISH,
                              (int) s_echo_payload_len, s_echo_payload );
                else
                {
                    ESP_LOGE( TAG, "Echo publish FAILED status=%d — reconnecting", (int) st );
                    break;  /* treat echo failure as fatal — reconnect cleanly */
                }
            }

            loop_count++;
            if( loop_count >= PUBLISH_INTERVAL_LOOPS )
            {
                loop_count = 0;
                pub_packet_id = MQTT_GetPacketId( &mqtt_ctx );
                status = MQTT_Publish( &mqtt_ctx, &pub_info, pub_packet_id );
                if( status == MQTTSuccess )
                    ESP_LOGI( TAG, "Published to %s (packetID=%u)",
                              TOPIC_PUBLISH, (unsigned) pub_packet_id );
                else
                {
                    ESP_LOGW( TAG, "MQTT_Publish failed: %d", (int) status );
                    break;
                }
            }
        }

        /* Clean disconnect before reconnect */
        MQTT_Disconnect( &mqtt_ctx );
        tcp_disconnect( &netCtx );
        ESP_LOGW( TAG, "MQTT session ended — reconnect in %d ms",
                  MQTT_RECONNECT_DELAY_MS );
        vTaskDelay( pdMS_TO_TICKS( MQTT_RECONNECT_DELAY_MS ) );

    } /* end outer reconnect loop */
}
