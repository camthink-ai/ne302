#include <stdio.h>
#include <string.h>
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/debug.h"
#include "Log/debug.h"
#include "ms_network.h"

/// @brief Network receive data
/// @param network_ Network handle
/// @param buf Receive buffer
/// @param len Receive length
/// @return Less than 0 indicates failure, greater than 0 indicates actual received length
static int ms_network_base_recv(void *network_, uint8_t *buf, size_t len)
{
    int ret = 0;
    size_t all_rlen = 0;
    uint32_t timeout_ms = MS_NETWORK_DEFAULT_TIMEOUT_MS;
    fd_set readfds;
    struct timeval tv;
    ms_network_handle_t network = (ms_network_handle_t)network_;
    if (network == NULL || buf == NULL || len == 0) return NET_ERR_INVALID_ARG;
    if (network->rx_timeout_ms > 0) timeout_ms = network->rx_timeout_ms;
    
    // LOG_DRV_DEBUG("ms_network_base_recv timeout = %d / %d.", timeout_ms, network->timeout_ms);
    xSemaphoreTake(network->rx_lock, portMAX_DELAY);

    if (network->sock_fd < 0) {
        ret = NET_ERR_INVALID_STATE;
        goto ms_network_recv_end;
    }

    do {
        FD_ZERO(&readfds);
        FD_SET(network->sock_fd, &readfds);
        if (all_rlen > 0) timeout_ms = MS_NETWORK_RECV_IDLE_TIMEOUT_MS;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ret = select(network->sock_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            ret = NET_ERR_SELECT;
            LOG_DRV_ERROR("Failed to select socket(socket = %d, errno = %d).", network->sock_fd, errno);
            goto ms_network_recv_end;
        }
        if (ret == 0) {
            if (all_rlen == 0) {
                // // LOG_DRV_DEBUG("Socket(%d) recv select timeout.", network->sock_fd);
                ret = NET_ERR_TIMEOUT;
            }
            goto ms_network_recv_end;
        }
        if (!FD_ISSET(network->sock_fd, &readfds)) {
            LOG_DRV_ERROR("Socket(%d) select result not set.", network->sock_fd);
            ret = NET_ERR_SELECT;
            goto ms_network_recv_end;
        }

        ret = recv(network->sock_fd, (buf + all_rlen), (len - all_rlen), 0);
        if (ret <= 0) {
            ret = NET_ERR_RECV;
            LOG_DRV_ERROR("Failed to recv data(socket = %d, errno = %d).", network->sock_fd, errno);
            goto ms_network_recv_end;
        }
        all_rlen += ret;
    } while (all_rlen < len);

ms_network_recv_end:
    xSemaphoreGive(network->rx_lock);
    if (all_rlen > 0) {
        // LOG_DRV_DEBUG("Socket(%d) received %d bytes.", network->sock_fd, all_rlen);
        return all_rlen;
    }
    return ret;
}

/// @brief Network receive data
/// @param network_ Network handle
/// @param buf Receive buffer
/// @param len Receive length
/// @param timeout_ms Timeout in milliseconds
/// @return Less than 0 indicates failure, greater than 0 indicates actual received length
//static int ms_network_base_recv_timeout(void *network_, uint8_t *buf, size_t len, uint32_t timeout_ms)
//{
//    ms_network_handle_t network = (ms_network_handle_t)network_;
//    if (timeout_ms > 0) network->timeout_ms = timeout_ms;
//
//    // LOG_DRV_DEBUG("ms_network_base_recv_timeout timeout = %d / %d.", timeout_ms, network->timeout_ms);
//    return ms_network_base_recv(network_, buf, len);
//}

/// @brief Network send data
/// @param network_ Network handle
/// @param buf Send buffer
/// @param len Send length
/// @return Less than 0 indicates failure, greater than 0 indicates actual sent length
static int ms_network_base_send(void *network_, const uint8_t *buf, size_t len)
{
    int ret = 0;
    size_t all_slen = 0;
#if defined(MS_NETWORK_ONCE_MAX_SEND_SIZE) && MS_NETWORK_ONCE_MAX_SEND_SIZE > 0
    size_t once_send_size = 0;
#endif
    uint32_t timeout_ms = MS_NETWORK_DEFAULT_TIMEOUT_MS, send_timeout_ms = 0;
    uint32_t start_tick = 0, now_tick = 0, diff_tick = 0;
    fd_set writefds;
    struct timeval tv;
    ms_network_handle_t network = (ms_network_handle_t)network_;
    if (network == NULL || buf == NULL || len == 0) return NET_ERR_INVALID_ARG;
    if (network->tx_timeout_ms > 0) timeout_ms = network->tx_timeout_ms;

    xSemaphoreTake(network->tx_lock, portMAX_DELAY);
    start_tick = xTaskGetTickCount();

    if (network->sock_fd < 0) {
        ret = NET_ERR_INVALID_STATE;
        goto ms_network_send_end;
    }

    do {
        now_tick = xTaskGetTickCount();
        diff_tick = (now_tick < start_tick) ? ((portMAX_DELAY - start_tick) + now_tick) : (now_tick - start_tick);
        if (pdTICKS_TO_MS(diff_tick) >= timeout_ms) {
            if (all_slen == 0) {
                LOG_DRV_ERROR("Socket(%d) send timeout.", network->sock_fd);
                ret = NET_ERR_TIMEOUT;
                goto ms_network_send_end;
            }
            send_timeout_ms = MS_NETWORK_LAST_SEND_TIMEOUT_MS;
        } else {
            send_timeout_ms = timeout_ms - pdTICKS_TO_MS(diff_tick);
            if (send_timeout_ms < MS_NETWORK_LAST_SEND_TIMEOUT_MS) {
                send_timeout_ms = MS_NETWORK_LAST_SEND_TIMEOUT_MS;
            }
        }
        FD_ZERO(&writefds);
        FD_SET(network->sock_fd, &writefds);
        tv.tv_sec = (send_timeout_ms) / 1000;
        tv.tv_usec = (send_timeout_ms % 1000) * 1000;
        ret = select(network->sock_fd + 1, NULL, &writefds, NULL, &tv);
        if (ret < 0) {
            ret = NET_ERR_SELECT;
            LOG_DRV_ERROR("Failed to select socket(socket = %d, errno = %d).", network->sock_fd, errno);
            goto ms_network_send_end;
        }
        if (ret == 0) {
            if (all_slen == 0) {
                LOG_DRV_ERROR("Socket(%d) send select timeout.", network->sock_fd);
                ret = NET_ERR_TIMEOUT;
            }
            goto ms_network_send_end;
        }
        if (!FD_ISSET(network->sock_fd, &writefds)) {
            LOG_DRV_ERROR("Socket(%d) select result not set.", network->sock_fd);
            ret = NET_ERR_SELECT;
            goto ms_network_send_end;
        }

        // printf("send len = %d / %d.\r\n", all_slen, len);
#if (defined(MS_NETWORK_ONCE_MAX_SEND_SIZE) && MS_NETWORK_ONCE_MAX_SEND_SIZE > 0)
        once_send_size = (len - all_slen) > MS_NETWORK_ONCE_MAX_SEND_SIZE ? MS_NETWORK_ONCE_MAX_SEND_SIZE : (len - all_slen);
        ret = send(network->sock_fd, (buf + all_slen), once_send_size, 0);
#else
        ret = send(network->sock_fd, (buf + all_slen), (len - all_slen), 0);
#endif
        if (ret <= 0) {
            ret = NET_ERR_SEND;
            LOG_DRV_ERROR("Failed to send data(socket = %d, errno = %d).", network->sock_fd, errno);
            goto ms_network_send_end;
        }
        all_slen += ret;
    } while (all_slen < len);
    
ms_network_send_end:
    xSemaphoreGive(network->tx_lock);
    if (all_slen > 0) {
        if (all_slen != len) {
            now_tick = xTaskGetTickCount();
            diff_tick = (now_tick < start_tick) ? ((portMAX_DELAY - start_tick) + now_tick) : (now_tick - start_tick);
            LOG_DRV_WARN("Socket(%d) sent %d/%d bytes, used time: %d ms.", network->sock_fd, all_slen, len, pdTICKS_TO_MS(diff_tick));
        }
        // LOG_DRV_DEBUG("Socket(%d) sent %d/%d bytes.", network->sock_fd, all_slen, len);
        return all_slen;
    }
    return ret;
}

static int ms_network_rng_func(void *ctx, unsigned char *buf, size_t len)
{
    (void) ctx;
    uint32_t random32;
    extern RNG_HandleTypeDef hrng;
    for (size_t n = 0; n < len; n += sizeof(uint32_t)) {
        if (HAL_RNG_GenerateRandomNumber(&hrng, &random32) == HAL_OK) {
            memcpy((char *) buf + n, &random32, n + sizeof(random32) > len ? len - n : sizeof(random32));
        } else return -1;
    }
    return 0;
}

//static void ms_network_debug_func(void *arg, int level, const char *file, int line, const char *log)
//{
//    (void) arg;
//    const char *fname = strrchr(file, '/');
//    if (fname == NULL) fname = strrchr(file, '\\');
//    if (fname == NULL) fname = file;
//    else fname++;
//    printf("[%d] %s(%d): %s\r\n", level, fname, line, log);
//}

/// @brief Network initialization
/// @param tls_config TLS configuration
/// @return Network handle
ms_network_handle_t ms_network_init(const network_tls_config_t *tls_config)
{
    int ret = 0, dlen = 0;
    ms_network_handle_t network = NULL;
    
    network = (ms_network_handle_t)hal_mem_alloc_large(sizeof(ms_network_t));
    if (network == NULL) return NULL;
    memset(network, 0, sizeof(ms_network_t));

    network->rx_lock = xSemaphoreCreateMutex();
    if (network->rx_lock == NULL) {
        hal_mem_free(network);
        return NULL;
    }

    network->tx_lock = xSemaphoreCreateMutex();
    if (network->tx_lock == NULL) {
        vSemaphoreDelete(network->rx_lock);
        hal_mem_free(network);
        return NULL;
    }
    xSemaphoreTake(network->rx_lock, portMAX_DELAY);
    xSemaphoreTake(network->tx_lock, portMAX_DELAY);
    network->sock_fd = -1;
    if (tls_config != NULL && (tls_config->ca_data || tls_config->client_cert_data || tls_config->client_key_data)) {
        
    #if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000 && \
    defined(MBEDTLS_PSA_CRYPTO_C)
        psa_crypto_init();  // https://github.com/Mbed-TLS/mbedtls/issues/9072#issuecomment-2084845711
    #endif
        mbedtls_ssl_init(&network->ssl);
        mbedtls_ssl_config_init(&network->ssl_conf);
        mbedtls_x509_crt_init(&network->cacert);
        mbedtls_x509_crt_init(&network->clicert);
        mbedtls_pk_init(&network->pkey);
        // mbedtls_entropy_init(&network->entropy);
        mbedtls_ctr_drbg_init(&network->ctr_drbg);
        
        ret = mbedtls_ssl_config_defaults(&network->ssl_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            LOG_DRV_ERROR("mbedtls_ssl_config_defaults failed. ret=%d", ret);
            goto ms_network_init_failed;
        }
        mbedtls_ssl_conf_authmode(&network->ssl_conf, tls_config->is_verify_hostname ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_rng(&network->ssl_conf, ms_network_rng_func, &network->ctr_drbg);

        if (tls_config->ca_data != NULL) {
            dlen = tls_config->ca_len;
            if (tls_config->ca_len == 0) dlen = strlen(tls_config->ca_data) + 1;
            ret = mbedtls_x509_crt_parse(&network->cacert, (const unsigned char *)tls_config->ca_data, dlen);
            if (ret != 0) {
                LOG_DRV_ERROR("mbedtls_x509_crt_parse(ca) failed. ret=%d", ret);
                goto ms_network_init_failed;
            }
        }
        mbedtls_ssl_conf_ca_chain(&network->ssl_conf, &network->cacert, NULL);

        if (tls_config->client_cert_data != NULL && tls_config->client_key_data != NULL) {
            
            dlen = tls_config->client_cert_len;
            if (tls_config->client_cert_len == 0) dlen = strlen(tls_config->client_cert_data);
            ret = mbedtls_x509_crt_parse(&network->clicert, (const unsigned char *)tls_config->client_cert_data, dlen);
            if (ret != 0) {
                LOG_DRV_ERROR("mbedtls_x509_crt_parse(client) failed. ret=%d", ret);
                goto ms_network_init_failed;
            }

            dlen = tls_config->client_key_len;
            if (tls_config->client_key_len == 0) dlen = strlen(tls_config->client_key_data);
            ret = mbedtls_pk_parse_key(&network->pkey, (const unsigned char *)tls_config->client_key_data, dlen, NULL, 0, ms_network_rng_func, NULL);
            if (ret != 0) {
                LOG_DRV_ERROR("mbedtls_pk_parse_key(client) failed. ret=%d", ret);
                goto ms_network_init_failed;
            }

            mbedtls_ssl_conf_own_cert(&network->ssl_conf, &network->clicert, &network->pkey);
        }

        // mbedtls_ssl_conf_dbg(&network->ssl_conf, ms_network_debug_func, NULL);
        // mbedtls_debug_set_threshold(1);

        ret = mbedtls_ssl_setup(&network->ssl, &network->ssl_conf);
        if (ret != 0) {
            LOG_DRV_ERROR("mbedtls_ssl_setup failed. ret=%d", ret);
            goto ms_network_init_failed;
        }

        mbedtls_ssl_set_bio(&network->ssl, network, ms_network_base_send, ms_network_base_recv, NULL);
        network->is_verify_hostname = tls_config->is_verify_hostname;
        network->tls_enable_flag = 1;
    }

    xSemaphoreGive(network->tx_lock);
    xSemaphoreGive(network->rx_lock);
    return network;
ms_network_init_failed:
    xSemaphoreGive(network->tx_lock);
    xSemaphoreGive(network->rx_lock);
    ms_network_deinit(network);
    return NULL;
}

/// @brief DNS resolution
/// @param host Hostname
/// @param ipaddr Output IP address
/// @return Error code
int ms_network_dns_parse(const char *host, uint8_t *ipaddr)
{
    struct hostent *he = NULL;
    struct in_addr **addr_list = NULL;
    if (host == NULL || ipaddr == NULL) return NET_ERR_INVALID_ARG;

    // Resolve hostname
    he = lwip_gethostbyname(host);
    if (he == NULL) {
        LOG_DRV_ERROR("Failed to resolve hostname: %s", host);
        return NET_ERR_DNS;
    }
    // Get IP address list
    addr_list = (struct in_addr **)he->h_addr_list;
    if (addr_list == NULL || addr_list[0] == NULL) {
        LOG_DRV_ERROR("No IP address found for host: %s", host);
        return NET_ERR_DNS;
    }
    memcpy(ipaddr, &addr_list[0]->s_addr, 4);
    return NET_ERR_OK;
}

/// @brief Network connection
/// @param network Network handle
/// @param host Hostname
/// @param port Port
/// @return Error code
int ms_network_connect(ms_network_handle_t network, const char *host, uint16_t port, uint32_t timeout_ms)
{
    int ret = 0;
    int flags = 0;
    int error_code = 0;
    socklen_t error_len = sizeof(error_code);
    fd_set writefds;
    struct timeval tv;
    struct hostent *he = NULL;
    struct in_addr **addr_list = NULL;
    struct sockaddr_in server_addr;
    if (network == NULL || host == NULL || port == 0) return NET_ERR_INVALID_ARG;

    // Resolve hostname
    he = lwip_gethostbyname(host);
    if (he == NULL) {
        LOG_DRV_ERROR("Failed to resolve hostname: %s", host);
        return NET_ERR_DNS;
    }
    // Get IP address list
    addr_list = (struct in_addr **)he->h_addr_list;
    if (addr_list == NULL || addr_list[0] == NULL) {
        LOG_DRV_ERROR("No IP address found for host: %s", host);
        return NET_ERR_DNS;
    }
//    ip4_addr_t ipaddr = {.addr = addr_list[0]->s_addr};
    // LOG_DRV_DEBUG("Connect to: %s:%d", ip4addr_ntoa((const ip4_addr_t *)&ipaddr), port);

    xSemaphoreTake(network->rx_lock, portMAX_DELAY);
    xSemaphoreTake(network->tx_lock, portMAX_DELAY);

    // Check socket state
    if (network->sock_fd >= 0) {
        close(network->sock_fd);
        network->sock_fd = -1;
    }

    // Create socket
    network->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (network->sock_fd < 0) {
        LOG_DRV_ERROR("Failed to create socket(ret = %d).", network->sock_fd);
        ret = NET_ERR_SOCKET;
        goto ms_network_connect_end;
    }

    // Set socket to non-blocking
    flags = fcntl(network->sock_fd, F_GETFL, 0);
    fcntl(network->sock_fd, F_SETFL, flags | O_NONBLOCK);

    // Connect to server
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = addr_list[0]->s_addr;
    server_addr.sin_port = htons(port);
    ret = connect(network->sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        LOG_DRV_ERROR("Failed to connect to server(socket = %d, ret = %d).", network->sock_fd, errno);
        ret = NET_ERR_CONN;
        goto ms_network_connect_end;
    }

    if (ret < 0 && errno == EINPROGRESS) {
        FD_ZERO(&writefds);
        FD_SET(network->sock_fd, &writefds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ret = select(network->sock_fd + 1, NULL, &writefds, NULL, &tv);
        if (ret < 0) {
            ret = NET_ERR_SELECT;
            LOG_DRV_ERROR("Failed to select socket(socket = %d, errno = %d).", network->sock_fd, errno);
            goto ms_network_connect_end;
        }
        if (ret == 0) {
            // LOG_DRV_DEBUG("Socket(%s) connect select timeout.", network->sock_fd);
            ret = NET_ERR_TIMEOUT;
            goto ms_network_connect_end;
        }
        if (!FD_ISSET(network->sock_fd, &writefds)) {
            LOG_DRV_ERROR("Socket(%s) select result not set.", network->sock_fd);
            ret = NET_ERR_SELECT;
            goto ms_network_connect_end;
        }

        ret = getsockopt(network->sock_fd, SOL_SOCKET, SO_ERROR, &error_code, &error_len);
        if (ret < 0 || error_code != 0) {
            LOG_DRV_ERROR("Failed to connect to server(socket = %d, ret = %d, error_code = %d).", network->sock_fd, ret, error_code);
            ret = NET_ERR_CONN;
            goto ms_network_connect_end;
        }

        ret = NET_ERR_OK;
    }

    if (ret != 0) goto ms_network_connect_end;
   // LOG_DRV_DEBUG("Socket(%d) connected to server: %s:%d", network->sock_fd, host, port);

    if (network->tls_enable_flag) {
        // Reset TLS session
        mbedtls_ssl_session_reset(&network->ssl);
        // Always set SNI — required by most modern servers even when hostname
        // verification is disabled.  Without SNI the server cannot select the
        // correct certificate and may send a fatal alert.
        ret = mbedtls_ssl_set_hostname(&network->ssl, host);
        if (ret != 0) {
            LOG_DRV_ERROR("TLS set hostname failed(ret = %d).", ret);
            ret = NET_ERR_TLS;
            goto ms_network_connect_end;
        }
        // TLS handshake
        if (timeout_ms < MS_NETWORK_DEFAULT_TIMEOUT_MS) {
            network->rx_timeout_ms = MS_NETWORK_DEFAULT_TIMEOUT_MS;
            network->tx_timeout_ms = MS_NETWORK_DEFAULT_TIMEOUT_MS;
        } else {
            network->rx_timeout_ms = timeout_ms;
            network->tx_timeout_ms = timeout_ms;
        }

        while ((ret = mbedtls_ssl_handshake(&network->ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                LOG_DRV_ERROR("TLS handshake failed(ret = %d).", ret);
                ret = NET_ERR_TLS_HANDSHAKE;
                goto ms_network_connect_end;
            }
        }
    }

ms_network_connect_end:
    if (ret < 0 && network->sock_fd >= 0) {
        close(network->sock_fd);
        network->sock_fd = -1;
    }
    xSemaphoreGive(network->tx_lock);
    xSemaphoreGive(network->rx_lock);
    return ret;
}

/// @brief Network receive data
/// @param network Network handle
/// @param buf Receive buffer
/// @param len Receive length
/// @param timeout Timeout in milliseconds
/// @return Less than 0 indicates failure, greater than 0 indicates actual received length
int ms_network_recv(ms_network_handle_t network, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    int ret = 0;
    size_t rlen = len;
    if (network == NULL) return NET_ERR_INVALID_ARG;
    network->rx_timeout_ms = timeout_ms;

    // LOG_DRV_DEBUG("ms_network_recv timeout = %d / %d.", timeout_ms, network->timeout_ms);
    if (network->tls_enable_flag) {
        ret = mbedtls_ssl_read(&network->ssl, buf, rlen);
        if (ret < NET_ERR_UNKNOWN) {
            LOG_DRV_ERROR("TLS read failed(ret = -0x%x).", -ret);
            ret = NET_ERR_TLS;
        }
    }
    else ret = ms_network_base_recv(network, buf, rlen);

    return ret;
}

/// @brief Network send data
/// @param network Network handle
/// @param buf Send buffer
/// @param len Send length
/// @param timeout Timeout in milliseconds
/// @return Less than 0 indicates failure, greater than 0 indicates actual sent length
int ms_network_send(ms_network_handle_t network, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    int ret = 0;
    size_t all_slen = 0, slen = 0;
    size_t ssl_max_out_len = 0;
    if (network == NULL) return NET_ERR_INVALID_ARG;

    network->tx_timeout_ms = timeout_ms;
    if (network->tls_enable_flag) {
        ssl_max_out_len = mbedtls_ssl_get_max_out_record_payload(&network->ssl);
        if (ssl_max_out_len <= 0) return NET_ERR_TLS;
        do {
            slen = (len - all_slen) > ssl_max_out_len ? ssl_max_out_len : (len - all_slen);
            ret = mbedtls_ssl_write(&network->ssl, buf + all_slen, slen);
            if (ret < 0) {
                if (all_slen > 0) return all_slen;
                else if (ret < NET_ERR_UNKNOWN) {
                    LOG_DRV_ERROR("TLS write failed(ret = -0x%x).", -ret);
                    ret = NET_ERR_TLS;
                }
                return ret;
            }
            all_slen += ret;
        } while (all_slen < len);
        return all_slen;
    } else {
        return ms_network_base_send(network, (const uint8_t *)buf, len);
    }
}

/// @brief Network close
/// @param network Network handle
void ms_network_close(ms_network_handle_t network)
{
    if (network == NULL) return;
    
    xSemaphoreTake(network->rx_lock, portMAX_DELAY);
    xSemaphoreTake(network->tx_lock, portMAX_DELAY);
    if (network->sock_fd >= 0) {
        if (network->tls_enable_flag) {
            mbedtls_ssl_close_notify(&network->ssl);
        }
        shutdown(network->sock_fd, SHUT_RDWR);
        close(network->sock_fd);
        network->sock_fd = -1;
    }
    xSemaphoreGive(network->tx_lock);
    xSemaphoreGive(network->rx_lock);
}

/// @brief Network deinitialize
/// @param network Network handle
void ms_network_deinit(ms_network_handle_t network)
{
    if (network == NULL) return;
    
    ms_network_close(network);
    xSemaphoreTake(network->rx_lock, portMAX_DELAY);
    xSemaphoreTake(network->tx_lock, portMAX_DELAY);
    if (network->tls_enable_flag) {
        mbedtls_x509_crt_free(&network->cacert);
        mbedtls_x509_crt_free(&network->clicert);
        mbedtls_pk_free(&network->pkey);
        mbedtls_ssl_free(&network->ssl);
        mbedtls_ssl_config_free(&network->ssl_conf);
        // mbedtls_ctr_drbg_free(&network->ctr_drbg);
        mbedtls_entropy_free(&network->entropy);
        network->tls_enable_flag = false;
    }
    vSemaphoreDelete(network->tx_lock);
    vSemaphoreDelete(network->rx_lock);
    hal_mem_free(network);
}
