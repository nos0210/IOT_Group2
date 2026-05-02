/*
 * Plain HTTP GET demo using coreHTTP on ESP32.
 *
 * Flow:
 *   1. Wait for Wi-Fi / IP (event group BIT_WIFI_UP set by network_service).
 *   2. DNS resolve "httpforever.com".
 *   3. Open a raw TCP socket to port 80.
 *   4. Build & send  GET / HTTP/1.1  via coreHTTP.
 *   5. Log status code, headers length, body length, and the first
 *      HTTP_LOG_BODY_CHUNK_SIZE bytes of the body.
 *   6. Close socket.
 */

#include "http_client_task.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

/* coreHTTP */
#include "core_http_client.h"

/* ── Configuration ──────────────────────────────────────────────────────── */
#define HTTP_SERVER_HOST        "httpforever.com"
#define HTTP_SERVER_PORT        80
#define HTTP_REQUEST_PATH       "/"

#define HTTP_REQUEST_BUF_SIZE   512U
#define HTTP_RESPONSE_BUF_SIZE  8192U

/* Maximum bytes of body to print per ESP_LOG call (avoids truncation). */
#define HTTP_LOG_BODY_CHUNK     128U

/* TCP receive/send timeout (seconds). */
#define TCP_TIMEOUT_SEC         10

/* Retry interval while waiting for Wi-Fi / valid IP. */
#define WIFI_WAIT_MS            1000

/* Maximum time to wait for a valid IP before giving up (ms). */
#define WIFI_WAIT_TIMEOUT_MS    60000

/* Number of DNS retry attempts before failing. */
#define DNS_RETRY_COUNT         5
#define DNS_RETRY_DELAY_MS      2000

/* Tag for ESP_LOG. */
static const char *TAG = "HTTP_CLIENT";

/* ── NetworkContext ─────────────────────────────────────────────────────── */

/*
 * coreHTTP requires  struct NetworkContext  to be defined by the application.
 * Here we only need the file descriptor of the connected TCP socket.
 */
struct NetworkContext
{
    int tcpSocket;
};

/* ── Transport callbacks ────────────────────────────────────────────────── */

static int32_t transport_recv( NetworkContext_t * pCtx,
                               void             * pBuffer,
                               size_t             bytesToRecv )
{
    int32_t received = ( int32_t ) recv( pCtx->tcpSocket, pBuffer,
                                         bytesToRecv, 0 );
    if( received == 0 )
    {
        /* Peer closed the connection. Signal error to coreHTTP. */
        return -1;
    }
    if( received < 0 )
    {
        if( errno == EAGAIN || errno == EWOULDBLOCK )
        {
            /* Timeout — return 0 so coreHTTP retries (uses getTime). */
            return 0;
        }
        ESP_LOGE( TAG, "recv() error: %d", errno );
        return -1;
    }
    return received;
}

static int32_t transport_send( NetworkContext_t * pCtx,
                               const void       * pBuffer,
                               size_t             bytesToSend )
{
    int32_t sent = ( int32_t ) send( pCtx->tcpSocket, pBuffer,
                                     bytesToSend, 0 );
    if( sent < 0 )
    {
        ESP_LOGE( TAG, "send() error: %d", errno );
    }
    return sent;
}

/* ── TCP helpers ────────────────────────────────────────────────────────── */

static int tcp_connect( NetworkContext_t * pCtx,
                        const char       * host,
                        uint16_t           port )
{
    /* ── DNS lookup ───────────────────────────────────────────────────── */
    struct addrinfo hints;
    memset( &hints, 0, sizeof( hints ) );
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[ 6 ];
    snprintf( port_str, sizeof( port_str ), "%u", ( unsigned ) port );

    struct addrinfo * res = NULL;
    int err = 0;
    for( int attempt = 1; attempt <= DNS_RETRY_COUNT; attempt++ )
    {
        ESP_LOGI( TAG, "DNS lookup: %s (attempt %d/%d) ...",
                  host, attempt, DNS_RETRY_COUNT );
        res = NULL;
        err = getaddrinfo( host, port_str, &hints, &res );
        if( err == 0 && res != NULL )
        {
            break;  /* success */
        }
        ESP_LOGW( TAG, "getaddrinfo failed: %d — retry in %d ms",
                  err, DNS_RETRY_DELAY_MS );
        if( res != NULL ) { freeaddrinfo( res ); res = NULL; }
        vTaskDelay( pdMS_TO_TICKS( DNS_RETRY_DELAY_MS ) );
    }
    if( err != 0 || res == NULL )
    {
        ESP_LOGE( TAG, "DNS lookup failed after %d attempts (err=%d)",
                  DNS_RETRY_COUNT, err );
        return -1;
    }

    /* Log the resolved IP address. */
    char ip_str[ 16 ];
    struct sockaddr_in * addr4 = ( struct sockaddr_in * ) res->ai_addr;
    inet_ntop( AF_INET, &addr4->sin_addr, ip_str, sizeof( ip_str ) );
    ESP_LOGI( TAG, "Resolved %s -> %s", host, ip_str );

    /* ── Create socket ────────────────────────────────────────────────── */
    int sock = socket( res->ai_family, res->ai_socktype, 0 );
    if( sock < 0 )
    {
        ESP_LOGE( TAG, "socket() failed: %d", errno );
        freeaddrinfo( res );
        return -1;
    }

    /* Set receive / send timeout so transport_recv can return 0 on timeout. */
    struct timeval tv = { .tv_sec = TCP_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
    setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );

    /* ── Connect ──────────────────────────────────────────────────────── */
    ESP_LOGI( TAG, "Connecting to %s:%d ...", host, port );
    if( connect( sock, res->ai_addr, res->ai_addrlen ) != 0 )
    {
        ESP_LOGE( TAG, "connect() failed: %d", errno );
        close( sock );
        freeaddrinfo( res );
        return -1;
    }

    freeaddrinfo( res );
    pCtx->tcpSocket = sock;
    ESP_LOGI( TAG, "TCP connected to %s:%d (fd=%d)", host, port, sock );
    return 0;
}

/* ── Task entry point ───────────────────────────────────────────────────── */

/* ── Wait until esp_netif has a non-zero IPv4 address ──────────────────── */

static bool wait_for_ip( uint32_t timeout_ms )
{
    uint32_t elapsed = 0;

    while( elapsed < timeout_ms )
    {
        /* Re-query each iteration: netif may not exist yet when task starts */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey( "WIFI_STA_DEF" );
        if( netif != NULL )
        {
            esp_netif_ip_info_t ip_info;
            if( esp_netif_get_ip_info( netif, &ip_info ) == ESP_OK &&
                ip_info.ip.addr != 0 )
            {
                ESP_LOGI( TAG, "Got IP: " IPSTR, IP2STR( &ip_info.ip ) );
                return true;
            }
        }
        vTaskDelay( pdMS_TO_TICKS( WIFI_WAIT_MS ) );
        elapsed += WIFI_WAIT_MS;
        ESP_LOGI( TAG, "Waiting for IP... (%lu/%lu ms)",
                  ( unsigned long ) elapsed,
                  ( unsigned long ) timeout_ms );
    }
    return false;
}

/* ── Task entry point ───────────────────────────────────────────────────── */

void http_client_task( void * pvParameters )
{
    ( void ) pvParameters;

    ESP_LOGI( TAG, "HTTP client task started — waiting for Wi-Fi IP..." );

    if( !wait_for_ip( WIFI_WAIT_TIMEOUT_MS ) )
    {
        ESP_LOGE( TAG, "Timed out waiting for IP — task exiting" );
        vTaskDelete( NULL );
        return;
    }

    /* ── Static buffers (kept off the tiny default stack) ─────────────── */
    static uint8_t requestBuffer [ HTTP_REQUEST_BUF_SIZE  ];
    static uint8_t responseBuffer[ HTTP_RESPONSE_BUF_SIZE ];

    /* ── Transport interface ──────────────────────────────────────────── */
    NetworkContext_t    networkCtx = { .tcpSocket = -1 };
    TransportInterface_t transport  =
    {
        .recv            = transport_recv,
        .send            = transport_send,
        .pNetworkContext = &networkCtx,
        .writev          = NULL,
    };

    /* ── TCP connect ──────────────────────────────────────────────────── */
    if( tcp_connect( &networkCtx, HTTP_SERVER_HOST, HTTP_SERVER_PORT ) != 0 )
    {
        ESP_LOGE( TAG, "TCP connect failed — task exiting" );
        vTaskDelete( NULL );
        return;
    }

    /* ── coreHTTP: build request headers ─────────────────────────────── */
    HTTPRequestInfo_t requestInfo =
    {
        .pMethod   = HTTP_METHOD_GET,
        .methodLen = sizeof( HTTP_METHOD_GET ) - 1U,
        .pPath     = HTTP_REQUEST_PATH,
        .pathLen   = sizeof( HTTP_REQUEST_PATH ) - 1U,
        .pHost     = HTTP_SERVER_HOST,
        .hostLen   = sizeof( HTTP_SERVER_HOST ) - 1U,
        .reqFlags  = 0U,          /* Connection: close (no Keep-Alive) */
    };

    HTTPRequestHeaders_t requestHeaders =
    {
        .pBuffer   = requestBuffer,
        .bufferLen = sizeof( requestBuffer ),
        .headersLen = 0U,
    };

    HTTPStatus_t status = HTTPClient_InitializeRequestHeaders( &requestHeaders,
                                                               &requestInfo );
    if( status != HTTPSuccess )
    {
        ESP_LOGE( TAG, "HTTPClient_InitializeRequestHeaders: %s",
                  HTTPClient_strerror( status ) );
        close( networkCtx.tcpSocket );
        vTaskDelete( NULL );
        return;
    }

    ESP_LOGI( TAG, "Request headers (%zu bytes):\n%.*s",
              requestHeaders.headersLen,
              ( int ) requestHeaders.headersLen,
              ( char * ) requestHeaders.pBuffer );

    /* ── coreHTTP: send request & receive response ────────────────────── */
    HTTPResponse_t response =
    {
        .pBuffer              = responseBuffer,
        .bufferLen            = sizeof( responseBuffer ),
        .pHeaderParsingCallback = NULL,
        .getTime              = NULL,
    };

    ESP_LOGI( TAG, "Sending GET %s HTTP/1.1 to %s ...",
              HTTP_REQUEST_PATH, HTTP_SERVER_HOST );

    status = HTTPClient_Send( &transport,
                              &requestHeaders,
                              NULL,   /* pRequestBodyBuf  — none for GET */
                              0U,     /* reqBodyBufLen                   */
                              &response,
                              0U );   /* sendFlags                       */

    /* ── Log result ───────────────────────────────────────────────────── */
    if( status == HTTPSuccess )
    {
        ESP_LOGI( TAG, "========== HTTP Response ===========" );
        ESP_LOGI( TAG, "Status Code : %u",   response.statusCode );
        ESP_LOGI( TAG, "Headers Len : %zu B", response.headersLen );
        ESP_LOGI( TAG, "Body Len    : %zu B", response.bodyLen    );

        /* --- Print response headers --- */
        ESP_LOGI( TAG, "--- Response Headers ---" );
        ESP_LOG_BUFFER_CHAR( TAG, response.pHeaders, response.headersLen );

        /* --- Print response body in chunks --- */
        ESP_LOGI( TAG, "--- Response Body ---" );
        size_t offset = 0U;
        while( offset < response.bodyLen )
        {
            size_t chunk = response.bodyLen - offset;
            if( chunk > HTTP_LOG_BODY_CHUNK )
            {
                chunk = HTTP_LOG_BODY_CHUNK;
            }
            ESP_LOG_BUFFER_CHAR( TAG, response.pBody + offset, chunk );
            offset += chunk;
        }
        ESP_LOGI( TAG, "====================================" );
    }
    else
    {
        ESP_LOGE( TAG, "HTTPClient_Send failed: %s (%d)",
                  HTTPClient_strerror( status ), ( int ) status );
    }

    /* ── Clean up ─────────────────────────────────────────────────────── */
    close( networkCtx.tcpSocket );
    ESP_LOGI( TAG, "Socket closed. HTTP task done." );

    vTaskDelete( NULL );
}
