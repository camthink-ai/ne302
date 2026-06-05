#include <string.h>
#include <stdio.h>
#include "Log/debug.h"
#include "storage.h"
#include "ms_mqtt_client.h"
#include "si91x_mqtt_client.h"
#include "ms_mqtt_client_test.h"

#define TEST_CA_PATH                "/mqtt_test_ca.crt"
#define TEST_CLIENT_CERT_PATH       "/mqtt_test_client.crt"
#define TEST_CLIENT_KEY_PATH        "/mqtt_test_client.key"

#define TEST_CA_DATA    "-----BEGIN CERTIFICATE-----\n\
MIIEAzCCAuugAwIBAgIUBY1hlCGvdj4NhBXkZ/uLUZNILAwwDQYJKoZIhvcNAQEL\n\
BQAwgZAxCzAJBgNVBAYTAkdCMRcwFQYDVQQIDA5Vbml0ZWQgS2luZ2RvbTEOMAwG\n\
A1UEBwwFRGVyYnkxEjAQBgNVBAoMCU1vc3F1aXR0bzELMAkGA1UECwwCQ0ExFjAU\n\
BgNVBAMMDW1vc3F1aXR0by5vcmcxHzAdBgkqhkiG9w0BCQEWEHJvZ2VyQGF0Y2hv\n\
by5vcmcwHhcNMjAwNjA5MTEwNjM5WhcNMzAwNjA3MTEwNjM5WjCBkDELMAkGA1UE\n\
BhMCR0IxFzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTES\n\
MBAGA1UECgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVp\n\
dHRvLm9yZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzCCASIwDQYJ\n\
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAME0HKmIzfTOwkKLT3THHe+ObdizamPg\n\
UZmD64Tf3zJdNeYGYn4CEXbyP6fy3tWc8S2boW6dzrH8SdFf9uo320GJA9B7U1FW\n\
Te3xda/Lm3JFfaHjkWw7jBwcauQZjpGINHapHRlpiCZsquAthOgxW9SgDgYlGzEA\n\
s06pkEFiMw+qDfLo/sxFKB6vQlFekMeCymjLCbNwPJyqyhFmPWwio/PDMruBTzPH\n\
3cioBnrJWKXc3OjXdLGFJOfj7pP0j/dr2LH72eSvv3PQQFl90CZPFhrCUcRHSSxo\n\
E6yjGOdnz7f6PveLIB574kQORwt8ePn0yidrTC1ictikED3nHYhMUOUCAwEAAaNT\n\
MFEwHQYDVR0OBBYEFPVV6xBUFPiGKDyo5V3+Hbh4N9YSMB8GA1UdIwQYMBaAFPVV\n\
6xBUFPiGKDyo5V3+Hbh4N9YSMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL\n\
BQADggEBAGa9kS21N70ThM6/Hj9D7mbVxKLBjVWe2TPsGfbl3rEDfZ+OKRZ2j6AC\n\
6r7jb4TZO3dzF2p6dgbrlU71Y/4K0TdzIjRj3cQ3KSm41JvUQ0hZ/c04iGDg/xWf\n\
+pp58nfPAYwuerruPNWmlStWAXf0UTqRtg4hQDWBuUFDJTuWuuBvEXudz74eh/wK\n\
sMwfu1HFvjy5Z0iMDU8PUDepjVolOCue9ashlS4EB5IECdSR2TItnAIiIwimx839\n\
LdUdRudafMu5T5Xma182OC0/u/xRlEm+tvKGGmfFcN0piqVl8OrSPBgIlb+1IKJE\n\
m/XriWr/Cq4h/JfB7NTsezVslgkBaoU=\n\
-----END CERTIFICATE-----\n"

#define TEST_CLIENT_CERTIFICATE "-----BEGIN CERTIFICATE-----\n\
MIIDtDCCApygAwIBAgIBADANBgkqhkiG9w0BAQsFADCBkDELMAkGA1UEBhMCR0Ix\n\
FzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTESMBAGA1UE\n\
CgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVpdHRvLm9y\n\
ZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzAeFw0yNTA5MDIwODM1\n\
MDZaFw0yNTEyMDEwODM1MDZaMIGNMRswGQYDVQQDDBJ0ZXN0Lm1vc3F1aXR0by5v\n\
cmcxCzAJBgNVBAoMAm1zMREwDwYDVQQLDAhjYW50aGluazEfMB0GCSqGSIb3DQEJ\n\
ARYQbGlqaEBjYW10aGluay5haTELMAkGA1UEBhMCSEsxDzANBgNVBAgMBmZ1amlh\n\
bjEPMA0GA1UEBwwGeGlhbWVuMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC\n\
AQEAuBgGrXq5Gp64yLEG2DZDF9Vo2kgu/2nXrNqSeBCO12CQgECq3Sg72CdVnVQy\n\
Pww9JZ0HpNzADlcyrwR/Ao8tPfaR4n/1mTzSblgVeUMyzgTDxVBTavsNlzS1UWRJ\n\
ubUFvunPwc8fD91L0NpgThBSQRvbtxN42+rOpmXPT74Au1KXAUlnfFDm5O1OIiUA\n\
Z6+YkBuBXlCdUGitezH4UJScMakyajqXP8XX0iGqN/nwl+voyOY3f834EnUPzEaS\n\
0Ddm5wtXzEYs9fXQ9KZ8+oD3eoZJo0ctf2F/LC6YdmmD1Kk8Nwi5d14G8MqvTeaz\n\
BClHjKJXih4r7c0tD/2Eld5v6QIDAQABoxowGDAJBgNVHRMEAjAAMAsGA1UdDwQE\n\
AwIF4DANBgkqhkiG9w0BAQsFAAOCAQEAM/w6XrL11mQ8LhHLy23fhz1fkGY5Cz5u\n\
pp4nFXx8jByVRXmHz6SiyOOIQAvFtID2F9nspb2p9DVDjnLxz33ewGkZM1Ejr9uz\n\
T7cqnRrV5I4hl0uluKpr1tn0QEczgvDIBiAz+qBD6ulGJW/VYA0q3ovnG8wB3EnA\n\
I3gIAXMiPshgq0dmWFRDxjP21ri4wdC3C0wFMYhkgXlltHOXzXepvRTzHfGWQ8pe\n\
fIykhc2SLzMmK3NmkxQq2lzDCykpTDt4xSLrAQ+CZ8+IAley6cd8zwoP/UaPE/Yf\n\
FTIySqaxkNkg2MGrO9+qt9BWZ0VxycW+s+Ou4wwhuG0sTAmXn7dEjg==\n\
-----END CERTIFICATE-----\n"

#define TEST_CLIENT_KEY "-----BEGIN PRIVATE KEY-----\n\
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC4GAaterkanrjI\n\
sQbYNkMX1WjaSC7/ades2pJ4EI7XYJCAQKrdKDvYJ1WdVDI/DD0lnQek3MAOVzKv\n\
BH8Cjy099pHif/WZPNJuWBV5QzLOBMPFUFNq+w2XNLVRZEm5tQW+6c/Bzx8P3UvQ\n\
2mBOEFJBG9u3E3jb6s6mZc9PvgC7UpcBSWd8UObk7U4iJQBnr5iQG4FeUJ1QaK17\n\
MfhQlJwxqTJqOpc/xdfSIao3+fCX6+jI5jd/zfgSdQ/MRpLQN2bnC1fMRiz19dD0\n\
pnz6gPd6hkmjRy1/YX8sLph2aYPUqTw3CLl3Xgbwyq9N5rMEKUeMoleKHivtzS0P\n\
/YSV3m/pAgMBAAECggEABHvApTbHrXRfoN2fgnesgcxWqc2XvWyFDjvi1M3nGKuM\n\
oV78ekyJoT2bVMH77klR75RSicCUOdUu/LsHN9dSUqpqX6HeUp18Aige2m2ZSLyu\n\
IuROVRzo9hLYdp4Suz/YybnqhM29SSw1IDtCnKwXlhKDsu1PDyC5vdeIFBOsc2No\n\
iiOka/VAUuQYYa0iQoDKvi64jgThF7TUvaSs/1PxUE1jdwhZcWrT9odLVo/DA2oB\n\
UnNtCMcQnQ6qwuKAMjzeroGQ8c0SJ6ODNHTSWDV45YLzBDlI9IPJOyh3xi4OV1IW\n\
YQh20MH16m+iZ/67nBfCikvNdgTk50UMGzq4xcV4HQKBgQDfZNn7mxbIqhUd/c1R\n\
5jRLw2VfE1+hgIvOYUxJFo+ekDmDgj1acimiJ1PxhW04H1fdimEtPksh+ZV9ttex\n\
MLf9GbULQB8kumkz7hKRmF/CvQNlvAhAa+rLjQDsw0P47yiZVeTaxqAnKsHs2Tds\n\
c8FYeT5XdoOxH1HPqcTd/j+ONQKBgQDS9rhxmQXI7n9JbVUBXeEpz7mBFh2s2E8b\n\
MvVkzcpzks7fnIOfq089iT2CggPQP2wODuImfhHzY6ozzf1ZuAb1PkfU3MYmEnZA\n\
FUohdRy2I78Ice/kAYBJKCKZ4silJ2HBX/SlKtz2ZGMc18/OH7KWtCtalHm0iPWo\n\
SPa+EGyhZQKBgFXkQuO4l6QDsSbc5MquhkDzGdDadBR2hkqNonUflYzTyylDNC/I\n\
YTWVhXMBaCMB+hiWEhMBNoYTnmS9nmTkZHmOHsv6lX7bpYNv7/fG7FrFrb658zpB\n\
w/8LceWWllXqLmP2YI21fPJSQEm+os6yGw7XXx7l1HCPqDb+AVGRZgJZAoGAFp3g\n\
tD0Fg78d4k9YV6cq5oKgjphCYi9me7IA4Oe3FqDckNiEu+9vtVcrQvyRUBgci31I\n\
/XtyjTdYHemtiZrTFCOzK7zneVltV/1wTxnxFA3NAyjD4RvQFwe4fer0O9B3CHYz\n\
EioAOmIUxhjU7HI1gTabl0Bns9UjEQGRglrcaokCgYEAxzfBhmOCNBdB1r/96Rov\n\
uuY/xk8ZfvdglK09yrMC48gSYeDN5on2iF/XNnu7P/+bs6UwhsGZ2hm8Z45jWDgW\n\
ZrzELQFj206gKklJJQ0tvOCX3djc9NOcE7ZyVDzZT0ERp7gWZLwQbjirJQkvdgtW\n\
apKhYrDq4MFVZ40QeGLCjUI=\n\
-----END PRIVATE KEY-----\n"

#define TEST_CLIENT_BUFFER_SIZE   (2048 * 1024)

/// @brief MQTT client handle
static ms_mqtt_client_handle_t mqtt_client = NULL;
/// @brief MQTT state string
static const char *mqttt_state_str_list[] = {
    "STOPPED",
    "STARTING",
    "DISCONNECTED",
    "CONNECTED",
    "WAIT_RECONNECT",
    "UNKNOWN",
};
/// @brief MQTT event string
static const char *mqttt_event_str_list[] = {
    "ERROR",
    "STARTED",
    "STOPPED",
    "CONNECTED",
    "DISCONNECTED",
    "SUBSCRIBED",
    "UNSUBSCRIBED",
    "PUBLISHED",
    "DATA",
    "BEFORE_CONNECT",
    "DELETED",
    "USER",
};
/// @brief Default configuration
static const ms_mqtt_config_t ms_mqtt_default_config = {
    .base = {
        .protocol_ver = 4,
        .hostname = "test.mosquitto.org",
        .port = 8884,
        .client_id = "ms_mqtt_client",
        .clean_session = 1,
        .keepalive = 60,
    },
    .authentication = {
        .username = NULL,
        .password = NULL,
        // Test certificate from file
        // .ca_path = TEST_CA_PATH,
        // .ca_data = NULL,
        // .ca_len = 0,
        // .client_cert_path = TEST_CLIENT_CERT_PATH,
        // .client_cert_data = NULL,
        // .client_cert_len = 0,
        // .client_key_path = TEST_CLIENT_KEY_PATH,
        // .client_key_data = NULL,
        // .client_key_len = 0,
        // Test certificate from data
        .ca_path = NULL,
        .ca_data = TEST_CA_DATA,
        .ca_len = sizeof(TEST_CA_DATA),
        .client_cert_path = NULL,
        .client_cert_data = TEST_CLIENT_CERTIFICATE,
        .client_cert_len = sizeof(TEST_CLIENT_CERTIFICATE),
        .client_key_path = NULL,
        .client_key_data = TEST_CLIENT_KEY,
        .client_key_len = sizeof(TEST_CLIENT_KEY),
        // Test signing certificate
        // .ca_path = NULL,
        // .ca_data = TEST_CA_DATA,
        // .ca_len = sizeof(TEST_CA_DATA),
        // .client_cert_path = NULL,
        // .client_cert_data = NULL,
        // .client_cert_len = 0,
        // .client_key_path = NULL,
        // .client_key_data = NULL,
        // .client_key_len = 0,
        .is_verify_hostname = 1,
    },
    .last_will = {
        .topic = "ne302/will/test",
        .msg = "last will message",
        .msg_len = 0,
        .qos = 1,
        .retain = 0,
    },
    .task = {
        .priority = 32,
        .stack_size = 1024,
    },
    .network = {
        .disable_auto_reconnect = 0,
        .outbox_limit = 10,
        .outbox_resend_interval_ms = 30000,
        .outbox_expired_timeout = 50000,
        .reconnect_interval_ms = 10000,
        .timeout_ms = 10000,
        .buffer_size = TEST_CLIENT_BUFFER_SIZE,
    },
};

static uint32_t pub_tick = 0, ack_tick = 0, diff_tick = 0;
static void mqtt_client_event_callback(ms_mqtt_event_data_t *event_data, void *user_args)
{
    LOG_SIMPLE("MQTT event: %s, msg_id: %d", mqttt_event_str_list[event_data->event_id], event_data->msg_id);
    if (event_data->event_id == MQTT_EVENT_DATA) {
        LOG_SIMPLE("dup: %d, qos: %d, retain: %d", event_data->dup, event_data->qos, event_data->retain);
        LOG_SIMPLE("topic: %.*s, data(%d): %.*s", event_data->topic_len, (char *)event_data->topic, event_data->data_len, event_data->data_len, (char *)event_data->data);
    } else if (event_data->event_id == MQTT_EVENT_CONNECTED) {
        LOG_SIMPLE("session_present: %d", event_data->session_present);
    } else if (event_data->event_id == MQTT_EVENT_ERROR) {
        LOG_SIMPLE("error_code: %d, connect_rsp_code: %d", event_data->error_code, event_data->connect_rsp_code);
    } else if (event_data->event_id == MQTT_EVENT_PUBLISHED && pub_tick != 0) {
        ack_tick = xTaskGetTickCount();
        diff_tick = ack_tick < pub_tick ? ((portMAX_DELAY - pub_tick) + ack_tick) : (ack_tick - pub_tick);
        LOG_SIMPLE("pub diff_tick: %u", diff_tick);
        pub_tick = 0;
    }
    LOG_SIMPLE("");
}

#define MQTT_CLIENT_TEST_HELP_STR   "Usage: mqtt [cmd] args\r\n\
cmd: init/deinit/start/stop/reconnect/disconnect/sub/unsub/pub/pub_buf/state\r\n\
init args: [hostname] [port] [client_id] [username] [password]\r\n\
sub args: [topic] [qos]\r\n\
unsub args: [topic]\r\n\
pub args: [topic] [data] [qos] [retain]\r\n\
pub_buf args: [topic] [buffer_size] [qos] [retain]\r\n\r\n"

int ms_mqtt_client_test_cmd_deal(int argc, char* argv[])
{
    int ret = MQTT_ERR_OK;
    int qos = 0, retain = 0;
    char *pub_data = NULL;
    int pub_buf_size = 0;
    void *fd = NULL;
    ms_mqtt_config_t *config = NULL;

    if (argc < 2) {
        printf(MQTT_CLIENT_TEST_HELP_STR);
        return -1;
    }

    if (strcmp(argv[1], "save_cert") == 0) {
        fd = flash_lfs_fopen(TEST_CA_PATH, "w");
        if (fd == NULL) {
            LOG_LIB_ERROR("Failed to open ca file!");
            return -1;
        }
        flash_lfs_fwrite(fd, TEST_CA_DATA, sizeof(TEST_CA_DATA));
        flash_lfs_fclose(fd);
        fd = NULL;
        fd = flash_lfs_fopen(TEST_CLIENT_CERT_PATH, "w");
        if (fd == NULL) {
            LOG_LIB_ERROR("Failed to open client cert file!");
            return -1;
        }
        flash_lfs_fwrite(fd, TEST_CLIENT_CERTIFICATE, sizeof(TEST_CLIENT_CERTIFICATE));
        flash_lfs_fclose(fd);
        fd = NULL;
        fd = flash_lfs_fopen(TEST_CLIENT_KEY_PATH, "w");
        if (fd == NULL) {
            LOG_LIB_ERROR("Failed to open client key file!");
            return -1;
        }
        flash_lfs_fwrite(fd, TEST_CLIENT_KEY, sizeof(TEST_CLIENT_KEY));
        flash_lfs_fclose(fd);
        fd = NULL;
        LOG_SIMPLE("Cert saved successfully.\r\n");
        
        return 0;
    } else if (strcmp(argv[1], "init") == 0) {
        if (mqtt_client != NULL) {
            LOG_SIMPLE("MQTT client has been initialized.\r\n");
            return -1;
        } else {
            config = (ms_mqtt_config_t *)hal_mem_alloc_large(sizeof(ms_mqtt_config_t));
            if (config == NULL) {
                LOG_LIB_ERROR("MQTT client config malloc failed!");
                return -1;
            }
            memcpy(config, &ms_mqtt_default_config, sizeof(ms_mqtt_config_t));
            if (argc > 2) {
                config->base.hostname = argv[2];
                if (argc > 3) config->base.port = atoi(argv[3]);
                if (argc > 4) config->base.client_id = argv[4];
                if (argc > 5) {
                    config->authentication.username = argv[5];
                    if (argc > 6) config->authentication.password = argv[6];
                    else config->authentication.password = "";
                }
                config->authentication.is_verify_hostname = 0;
                config->authentication.ca_data = NULL;
                config->authentication.ca_len = 0;
                config->authentication.client_cert_data = NULL;
                config->authentication.client_cert_len = 0;
                config->authentication.client_key_data = NULL;
                config->authentication.client_key_len = 0;
            }
            mqtt_client = ms_mqtt_client_init(config);
            hal_mem_free(config);
            if (mqtt_client == NULL) ret = MQTT_ERR_MEM;
            else {
                ms_mqtt_client_register_event(mqtt_client, mqtt_client_event_callback, NULL);
                LOG_SIMPLE("MQTT client initialized.\r\n");
            }
        }
    } else {
        if (mqtt_client == NULL) {
            LOG_SIMPLE("MQTT client has not been initialized.\r\n");
            return -1;
        }

        if (strcmp(argv[1], "start") == 0) {
            ret = ms_mqtt_client_start(mqtt_client);
        } else if (strcmp(argv[1], "reconnect") == 0) {
            ret = ms_mqtt_client_reconnect(mqtt_client);
        } else if (strcmp(argv[1], "disconnect") == 0) {
            ret = ms_mqtt_client_disconnect(mqtt_client);
        } else if (strcmp(argv[1], "stop") == 0) {
            ret = ms_mqtt_client_stop(mqtt_client);
        } else if (strcmp(argv[1], "deinit") == 0) {
            ret = ms_mqtt_client_destroy(mqtt_client);
            if (ret == 0) mqtt_client = NULL;
        } else if (strcmp(argv[1], "state") == 0) {
            LOG_SIMPLE("MQTT state: %s\r\n", mqttt_state_str_list[ms_mqtt_client_get_state(mqtt_client)]);
            return 0;
        } else if (strcmp(argv[1], "sub") == 0) {
            if (argc < 3) {
                printf(MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                if (argc > 3) qos = atoi(argv[3]);
                ret = ms_mqtt_client_subscribe_single(mqtt_client, argv[2], qos);
            }
        } else if (strcmp(argv[1], "unsub") == 0) {
            if (argc < 3) {
                printf(MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                ret = ms_mqtt_client_unsubscribe(mqtt_client, argv[2]);
            }
        } else if (strcmp(argv[1], "pub") == 0) {
            if (argc < 3) {
                printf(MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                if (argc > 3) pub_data = argv[3];
                if (argc > 4) qos = atoi(argv[4]);
                if (argc > 5) retain = atoi(argv[5]);
                ret = ms_mqtt_client_publish(mqtt_client, argv[2], (uint8_t *)pub_data, strlen(pub_data), qos, retain);
            }
        } else if (strcmp(argv[1], "pub_buf") == 0) {
            if (argc < 3) {
                printf(MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                if (argc > 3) pub_buf_size = atoi(argv[3]);
                if (argc > 4) qos = atoi(argv[4]);
                if (argc > 5) retain = atoi(argv[5]);
                if (pub_buf_size <= 0 || pub_buf_size > TEST_CLIENT_BUFFER_SIZE) {
                    LOG_LIB_ERROR("Invalid pub buffer size!");
                    return -1;
                }

                pub_data = hal_mem_alloc_large(pub_buf_size);
                if (pub_data == NULL) {
                    LOG_LIB_ERROR("Memory alloc failed!");
                    return -1;
                }
                memset(pub_data, '#', pub_buf_size);
                
                pub_tick = xTaskGetTickCount();
                ret = ms_mqtt_client_publish(mqtt_client, argv[2], (uint8_t *)pub_data, pub_buf_size, qos, retain);
                if (ret <= 0) pub_tick = 0;
                hal_mem_free(pub_data);
                pub_data = NULL;
            }
        } else {
            LOG_SIMPLE("Invalid mqtt cmd: %s\r\n", argv[1]);
            return -1;
        }
        
        LOG_SIMPLE("MQTT cmd(%s) run result: %d\r\n", argv[1], ret);
    }
    return ret;
}

#define SI91X_MQTT_CLIENT_TEST_HELP_STR   "Usage: si91x_mqtt [cmd] args\r\n\
cmd: init/deinit/connect/disconnect/sub/unsub/pub/state\r\n\
init args: [hostname] [port] [client_id] [username] [password]\r\n\
sub args: [topic] [qos]\r\n\
unsub args: [topic]\r\n\
pub args: [topic] [data] [qos] [retain]\r\n\r\n"

static ms_mqtt_config_t *si91x_mqtt_config = NULL;

int si91x_mqtt_client_test_cmd_deal(int argc, char *argv[])
{
    int ret = MQTT_ERR_OK;
    int qos = 0, retain = 0;
    char *pub_data = "";
    

    if (argc < 2) {
        printf(SI91X_MQTT_CLIENT_TEST_HELP_STR);
        return -1;
    }

    if (strcmp(argv[1], "init") == 0) {
        if (si91x_mqtt_client_get_state() != MQTT_STATE_STOPPED) {
            LOG_SIMPLE("MQTT client has been initialized.\r\n");
            return -1;
        } else {
            if (si91x_mqtt_config == NULL) si91x_mqtt_config = (ms_mqtt_config_t *)hal_mem_alloc_large(sizeof(ms_mqtt_config_t));
            if (si91x_mqtt_config == NULL) {
                LOG_LIB_ERROR("MQTT client config malloc failed!");
                return -1;
            }
            memcpy(si91x_mqtt_config, &ms_mqtt_default_config, sizeof(ms_mqtt_config_t));
            if (argc > 2) {
                si91x_mqtt_config->base.hostname = argv[2];
                if (argc > 3) si91x_mqtt_config->base.port = atoi(argv[3]);
                if (argc > 4) si91x_mqtt_config->base.client_id = argv[4];
                if (argc > 5) {
                    si91x_mqtt_config->authentication.username = argv[5];
                    if (argc > 6) si91x_mqtt_config->authentication.password = argv[6];
                    else si91x_mqtt_config->authentication.password = "";
                }
                si91x_mqtt_config->authentication.is_verify_hostname = 0;
                si91x_mqtt_config->authentication.ca_data = NULL;
                si91x_mqtt_config->authentication.ca_len = 0;
                si91x_mqtt_config->authentication.client_cert_data = NULL;
                si91x_mqtt_config->authentication.client_cert_len = 0;
                si91x_mqtt_config->authentication.client_key_data = NULL;
                si91x_mqtt_config->authentication.client_key_len = 0;
            }
            ret = si91x_mqtt_client_init(si91x_mqtt_config);
            if (ret == MQTT_ERR_OK) {
                si91x_mqtt_client_register_event(mqtt_client_event_callback, NULL);
                LOG_SIMPLE("MQTT client initialized.\r\n");
            }
        }
    } else {
        if (si91x_mqtt_client_get_state() == MQTT_STATE_STOPPED) {
            LOG_SIMPLE("MQTT client has not been initialized.\r\n");
            return -1;
        }

        if (strcmp(argv[1], "connect") == 0) {
            ret = si91x_mqtt_client_connnect();
        } else if (strcmp(argv[1], "disconnect") == 0) {
            ret = si91x_mqtt_client_disconnect();
        } else if (strcmp(argv[1], "deinit") == 0) {
            ret = si91x_mqtt_client_deinit();
            if (ret == 0 && si91x_mqtt_config) {
                hal_mem_free(si91x_mqtt_config);
                si91x_mqtt_config = NULL;
            }
        } else if (strcmp(argv[1], "state") == 0) {
            LOG_SIMPLE("MQTT state: %s\r\n", mqttt_state_str_list[si91x_mqtt_client_get_state()]);
            return 0;
        } else if (strcmp(argv[1], "sub") == 0) {
            if (argc < 3) {
                printf(SI91X_MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                if (argc > 3) qos = atoi(argv[3]);
                ret = si91x_mqtt_client_subscribe(argv[2], qos);
            }
        } else if (strcmp(argv[1], "unsub") == 0) {
            if (argc < 3) {
                printf(SI91X_MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                ret = si91x_mqtt_client_unsubscribe(argv[2]);
            }
        } else if (strcmp(argv[1], "pub") == 0) {
            if (argc < 3) {
                printf(SI91X_MQTT_CLIENT_TEST_HELP_STR);
                return -1;
            } else {
                if (argc > 3) pub_data = argv[3];
                if (argc > 4) qos = atoi(argv[4]);
                if (argc > 5) retain = atoi(argv[5]);
                ret = si91x_mqtt_client_publish(argv[2], pub_data, strlen(pub_data), qos, retain);
            }
        } else {
            LOG_SIMPLE("Invalid mqtt cmd: %s\r\n", argv[1]);
            return -1;
        }
        
        LOG_SIMPLE("MQTT cmd(%s) run result: %d\r\n", argv[1], ret);
    }
    return ret;
}

debug_cmd_reg_t ms_mqtt_client_test_cmd_table[] = {
    {"mqtt",    "test mqtt module.",      ms_mqtt_client_test_cmd_deal},
    {"si91x_mqtt",    "test si91x mqtt module.",     si91x_mqtt_client_test_cmd_deal},
};

static void ms_mqtt_client_test_cmd_register(void)
{
    debug_cmdline_register(ms_mqtt_client_test_cmd_table, sizeof(ms_mqtt_client_test_cmd_table) / sizeof(ms_mqtt_client_test_cmd_table[0]));
}

void ms_mqtt_client_test_register(void)
{
    driver_cmd_register_callback("mqtt_test", ms_mqtt_client_test_cmd_register);
}
