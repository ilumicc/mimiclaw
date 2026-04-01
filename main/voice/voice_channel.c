#include "voice/voice_channel.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "nvs.h"
#include "cJSON.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "voice/wake_service.h"
#include "voice/voice_session_coordinator.h"

static const char *TAG = "voice";

#define VOICE_DEFAULT_CHAT_ID "local"

typedef struct {
    char ws_url[256];
    char ws_token[192];
    char ws_version[32];
    char device_id[32];
} voice_config_t;

static voice_config_t s_cfg;
static bool s_init_done = false;
static bool s_connected = false;

static esp_websocket_client_handle_t s_client = NULL;
static char s_ws_headers[512] = {0};

static void make_device_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err == ESP_OK) {
        snprintf(out, out_size, "mimi-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(out, out_size, "mimi-unknown");
    }
}

static esp_err_t nvs_load_str(const char *ns, const char *key, char *out, size_t out_size)
{
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = out_size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_save_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, key, value ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static void load_voice_config(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    snprintf(s_cfg.ws_url, sizeof(s_cfg.ws_url), "%s", MIMI_SECRET_VOICE_WS_URL);
    snprintf(s_cfg.ws_token, sizeof(s_cfg.ws_token), "%s", MIMI_SECRET_VOICE_WS_TOKEN);
    snprintf(s_cfg.ws_version, sizeof(s_cfg.ws_version), "%s", MIMI_SECRET_VOICE_WS_VERSION);

    if (nvs_load_str(MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_WS_URL,
                     s_cfg.ws_url, sizeof(s_cfg.ws_url)) == ESP_OK) {
        ESP_LOGI(TAG, "voice ws url loaded from NVS");
    }

    if (nvs_load_str(MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_WS_TOKEN,
                     s_cfg.ws_token, sizeof(s_cfg.ws_token)) == ESP_OK) {
        ESP_LOGI(TAG, "voice ws token loaded from NVS");
    }

    if (nvs_load_str(MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_WS_VER,
                     s_cfg.ws_version, sizeof(s_cfg.ws_version)) == ESP_OK) {
        ESP_LOGI(TAG, "voice ws version loaded from NVS");
    }

    if (!s_cfg.ws_version[0]) {
        snprintf(s_cfg.ws_version, sizeof(s_cfg.ws_version), "v1");
    }

    make_device_id(s_cfg.device_id, sizeof(s_cfg.device_id));
}

bool voice_channel_is_enabled(void)
{
    return s_cfg.ws_url[0] != '\0';
}

bool voice_channel_is_connected(void)
{
    return s_connected;
}

static esp_err_t voice_send_json_obj(cJSON *obj)
{
    if (!s_client || !obj) {
        return ESP_ERR_INVALID_STATE;
    }

    char *payload = cJSON_PrintUnformatted(obj);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(s_client, payload, strlen(payload), pdMS_TO_TICKS(1000));
    free(payload);

    return (sent > 0) ? ESP_OK : ESP_FAIL;
}

static void push_stt_to_inbound(const char *chat_id, const char *text)
{
    if (!text || !text[0]) {
        return;
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, (chat_id && chat_id[0]) ? chat_id : VOICE_DEFAULT_CHAT_ID,
            sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);

    if (!msg.content) {
        ESP_LOGE(TAG, "No memory for STT message");
        return;
    }

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Inbound queue full, dropping voice message");
        free(msg.content);
        return;
    }

    ESP_LOGI(TAG, "Voice STT forwarded to bus (%s): %.64s", msg.chat_id, text);
}

static void handle_json_message(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "Voice WS received invalid JSON");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (!cJSON_IsString(text)) {
            text = cJSON_GetObjectItem(root, "content");
        }
        if (!cJSON_IsString(text)) {
            text = cJSON_GetObjectItem(root, "transcript");
        }

        cJSON *chat_id = cJSON_GetObjectItem(root, "chat_id");
        cJSON *is_final = cJSON_GetObjectItem(root, "is_final");
        cJSON *final = cJSON_GetObjectItem(root, "final");

        bool final_flag = cJSON_IsTrue(is_final) || cJSON_IsTrue(final);
        if (!is_final && !final) {
            final_flag = true;
        }

        if (final_flag && cJSON_IsString(text) && text->valuestring[0]) {
            push_stt_to_inbound(cJSON_IsString(chat_id) ? chat_id->valuestring : NULL,
                                text->valuestring);
            voice_session_coordinator_on_transcript_final(text->valuestring);
        }
    } else if (strcmp(type->valuestring, "wake") == 0) {
        cJSON *keyword = cJSON_GetObjectItem(root, "keyword");
        if (!cJSON_IsString(keyword)) {
            keyword = cJSON_GetObjectItem(root, "text");
        }
        if (!cJSON_IsString(keyword)) {
            keyword = cJSON_GetObjectItem(root, "content");
        }

        const char *phrase = cJSON_IsString(keyword) ? keyword->valuestring : "wake";
        esp_err_t wake_err = wake_service_notify_detected("voice_ws", phrase);
        if (wake_err != ESP_OK) {
            ESP_LOGW(TAG, "Wake event ignored: %s", esp_err_to_name(wake_err));
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *message = cJSON_GetObjectItem(root, "message");
        ESP_LOGW(TAG, "Voice server error: %s", cJSON_IsString(message) ? message->valuestring : "unknown");
    }

    cJSON_Delete(root);
}

static void voice_ws_event_handler(void *handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "Voice WebSocket connected");

        cJSON *hello = cJSON_CreateObject();
        if (hello) {
            cJSON_AddStringToObject(hello, "type", "hello");
            cJSON_AddStringToObject(hello, "device_id", s_cfg.device_id);
            cJSON_AddStringToObject(hello, "version", s_cfg.ws_version);
            if (voice_send_json_obj(hello) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send hello");
            }
            cJSON_Delete(hello);
        }
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "Voice WebSocket disconnected");
        return;
    }

    if (event_id != WEBSOCKET_EVENT_DATA || !data || data->data_len <= 0) {
        return;
    }

    if (data->op_code != 0x1) {
        ESP_LOGD(TAG, "Ignore non-text voice frame op=%d", data->op_code);
        return;
    }

    char *payload = calloc(1, data->data_len + 1);
    if (!payload) {
        return;
    }

    memcpy(payload, data->data_ptr, data->data_len);
    handle_json_message(payload);
    free(payload);
}

esp_err_t voice_channel_init(void)
{
    load_voice_config();
    s_init_done = true;

    if (!voice_channel_is_enabled()) {
        ESP_LOGI(TAG, "Voice disabled: set voice_ws_url to enable");
    }

    return ESP_OK;
}

esp_err_t voice_channel_set_ws_url(const char *url)
{
    if (!url) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_save_str(MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_WS_URL, url);
    if (err == ESP_OK) {
        snprintf(s_cfg.ws_url, sizeof(s_cfg.ws_url), "%s", url);
    }
    return err;
}

esp_err_t voice_channel_set_ws_token(const char *token)
{
    if (!token) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_save_str(MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_WS_TOKEN, token);
    if (err == ESP_OK) {
        snprintf(s_cfg.ws_token, sizeof(s_cfg.ws_token), "%s", token);
    }
    return err;
}

esp_err_t voice_channel_set_ws_version(const char *version)
{
    if (!version || !version[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_save_str(MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_WS_VER, version);
    if (err == ESP_OK) {
        snprintf(s_cfg.ws_version, sizeof(s_cfg.ws_version), "%s", version);
    }
    return err;
}


esp_err_t voice_channel_start(void)
{
    if (!s_init_done) {
        esp_err_t init_err = voice_channel_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (!voice_channel_is_enabled()) {
        return ESP_OK;
    }

    if (s_client) {
        return ESP_OK;
    }

    s_ws_headers[0] = '\0';
    if (s_cfg.ws_token[0]) {
        snprintf(s_ws_headers, sizeof(s_ws_headers),
                 "Authorization: Bearer %s\r\n"
                 "X-Mimi-Voice-Version: %s\r\n"
                 "X-Mimi-Device-Id: %s\r\n",
                 s_cfg.ws_token, s_cfg.ws_version, s_cfg.device_id);
    } else {
        snprintf(s_ws_headers, sizeof(s_ws_headers),
                 "X-Mimi-Voice-Version: %s\r\n"
                 "X-Mimi-Device-Id: %s\r\n",
                 s_cfg.ws_version, s_cfg.device_id);
    }

    const esp_websocket_client_config_t ws_cfg = {
        .uri = s_cfg.ws_url,
        .headers = s_ws_headers,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 3000,
        .buffer_size = 2048,
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create voice websocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, voice_ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start voice websocket: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Voice channel started: %s", s_cfg.ws_url);
    return ESP_OK;
}

esp_err_t voice_channel_stop(void)
{
    if (!s_client) {
        return ESP_OK;
    }

    s_connected = false;
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;

    ESP_LOGI(TAG, "Voice channel stopped");
    return ESP_OK;
}

esp_err_t voice_channel_send_text(const char *chat_id, const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_client || !voice_channel_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "chat_id", (chat_id && chat_id[0]) ? chat_id : VOICE_DEFAULT_CHAT_ID);
    cJSON_AddStringToObject(root, "version", s_cfg.ws_version);
    cJSON_AddStringToObject(root, "device_id", s_cfg.device_id);

    esp_err_t err = voice_send_json_obj(root);
    cJSON_Delete(root);
    return err;
}
