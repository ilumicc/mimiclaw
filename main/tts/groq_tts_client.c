#include "tts/groq_tts_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

#include "mimi_config.h"
#include "proxy/http_proxy.h"

static const char *TAG = "groq_tts";

#define GROQ_TTS_API_HOST        "api.groq.com"
#define GROQ_TTS_API_URL         "https://api.groq.com/openai/v1/audio/speech"
#define GROQ_TTS_API_PATH        "/openai/v1/audio/speech"
#define GROQ_TTS_API_KEY_MAX     192
#define GROQ_TTS_MODEL_MAX       64
#define GROQ_TTS_VOICE_MAX       32

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} bin_buf_t;

static char s_api_key[GROQ_TTS_API_KEY_MAX] = {0};
static char s_model[GROQ_TTS_MODEL_MAX] = MIMI_SECRET_TTS_MODEL;
static char s_voice[GROQ_TTS_VOICE_MAX] = MIMI_SECRET_TTS_VOICE;

static void str_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static esp_err_t nvs_save_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_TTS, NVS_READWRITE, &nvs);
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

static esp_err_t bin_buf_init(bin_buf_t *buf, size_t initial_cap)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    buf->data = calloc(1, initial_cap);
    if (!buf->data) {
        return ESP_ERR_NO_MEM;
    }
    buf->len = 0;
    buf->cap = initial_cap;
    return ESP_OK;
}

static void bin_buf_free(bin_buf_t *buf)
{
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static esp_err_t bin_buf_append(bin_buf_t *buf, const void *data, size_t len)
{
    if (!buf || !data || len == 0) {
        return ESP_OK;
    }

    if (!buf->data || buf->cap == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    while (buf->len + len > buf->cap) {
        size_t new_cap = buf->cap * 2;
        uint8_t *tmp = realloc(buf->data, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return ESP_OK;
}

static ssize_t find_bytes(const uint8_t *haystack, size_t hay_len, const uint8_t *needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || hay_len < needle_len) {
        return -1;
    }

    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static bool wav_header_seems_valid(const uint8_t *data, size_t len)
{
    if (!data || len < 12) {
        return false;
    }
    return memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    bin_buf_t *buf = (bin_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return bin_buf_append(buf, evt->data, evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t groq_http_direct(const char *payload, bin_buf_t *buf, int *out_status)
{
    esp_http_client_config_t config = {
        .url = GROQ_TTS_API_URL,
        .event_handler = http_event_handler,
        .user_data = buf,
        .timeout_ms = 45000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    char auth_header[GROQ_TTS_API_KEY_MAX + 16];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_api_key);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    if (out_status) {
        *out_status = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t groq_http_proxy(const char *payload, bin_buf_t *body_buf, int *out_status)
{
    proxy_conn_t *conn = proxy_conn_open(GROQ_TTS_API_HOST, 443, 45000);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[1024];
    int header_len = snprintf(
        header, sizeof(header),
        "POST " GROQ_TTS_API_PATH " HTTP/1.1\r\n"
        "Host: " GROQ_TTS_API_HOST "\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        s_api_key, (int)strlen(payload));

    if (proxy_conn_write(conn, header, header_len) < 0 ||
        proxy_conn_write(conn, payload, strlen(payload)) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    bin_buf_t resp;
    esp_err_t err = bin_buf_init(&resp, 4096);
    if (err != ESP_OK) {
        proxy_conn_close(conn);
        return err;
    }

    uint8_t tmp[2048];
    while (1) {
        int n = proxy_conn_read(conn, (char *)tmp, sizeof(tmp), 45000);
        if (n <= 0) {
            break;
        }
        err = bin_buf_append(&resp, tmp, (size_t)n);
        if (err != ESP_OK) {
            break;
        }
    }
    proxy_conn_close(conn);

    if (err != ESP_OK) {
        bin_buf_free(&resp);
        return err;
    }

    const uint8_t sep[] = {'\r','\n','\r','\n'};
    ssize_t sep_idx = find_bytes(resp.data, resp.len, sep, sizeof(sep));
    if (sep_idx < 0) {
        bin_buf_free(&resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_status) {
        int status = 0;
        char status_line[64] = {0};
        const uint8_t *line_end = memchr(resp.data, '\n', resp.len);
        if (line_end) {
            size_t line_len = (size_t)(line_end - resp.data);
            if (line_len >= sizeof(status_line)) line_len = sizeof(status_line) - 1;
            memcpy(status_line, resp.data, line_len);
            sscanf(status_line, "HTTP/%*s %d", &status);
        }
        *out_status = status;
    }

    size_t body_off = (size_t)sep_idx + sizeof(sep);
    size_t body_len = (body_off <= resp.len) ? (resp.len - body_off) : 0;
    err = bin_buf_append(body_buf, resp.data + body_off, body_len);
    bin_buf_free(&resp);
    return err;
}

static esp_err_t build_payload_json(const char *text, char **out_json)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", s_model);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", s_voice);
    cJSON_AddStringToObject(root, "response_format", "wav");

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!*out_json) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t groq_tts_client_init(void)
{
    if (MIMI_SECRET_GROQ_API_KEY[0]) {
        str_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_GROQ_API_KEY);
    }
    if (MIMI_SECRET_TTS_MODEL[0]) {
        str_copy(s_model, sizeof(s_model), MIMI_SECRET_TTS_MODEL);
    }
    if (MIMI_SECRET_TTS_VOICE[0]) {
        str_copy(s_voice, sizeof(s_voice), MIMI_SECRET_TTS_VOICE);
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TTS, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[256] = {0};
        size_t len = sizeof(tmp);

        if (nvs_get_str(nvs, MIMI_NVS_KEY_GROQ_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            str_copy(s_api_key, sizeof(s_api_key), tmp);
        }

        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TTS_MODEL, tmp, &len) == ESP_OK && tmp[0]) {
            str_copy(s_model, sizeof(s_model), tmp);
        }

        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TTS_VOICE, tmp, &len) == ESP_OK && tmp[0]) {
            str_copy(s_voice, sizeof(s_voice), tmp);
        }

        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Groq TTS init: configured=%s, model=%s, voice=%s",
             groq_tts_is_configured() ? "yes" : "no",
             s_model,
             s_voice);

    return ESP_OK;
}

esp_err_t groq_tts_set_api_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_save_str(MIMI_NVS_KEY_GROQ_API_KEY, api_key);
    if (err == ESP_OK) {
        str_copy(s_api_key, sizeof(s_api_key), api_key);
    }
    return err;
}

esp_err_t groq_tts_set_model(const char *model)
{
    if (!model || !model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_save_str(MIMI_NVS_KEY_TTS_MODEL, model);
    if (err == ESP_OK) {
        str_copy(s_model, sizeof(s_model), model);
    }
    return err;
}

esp_err_t groq_tts_set_voice(const char *voice)
{
    if (!voice || !voice[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_save_str(MIMI_NVS_KEY_TTS_VOICE, voice);
    if (err == ESP_OK) {
        str_copy(s_voice, sizeof(s_voice), voice);
    }
    return err;
}

bool groq_tts_is_configured(void)
{
    return s_api_key[0] != '\0';
}

const char *groq_tts_get_model(void)
{
    return s_model;
}

const char *groq_tts_get_voice(void)
{
    return s_voice;
}

esp_err_t groq_tts_synthesize_wav(const char *text, uint8_t **out_wav, size_t *out_len)
{
    if (!text || !out_wav || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_wav = NULL;
    *out_len = 0;

    if (!groq_tts_is_configured()) {
        ESP_LOGW(TAG, "Groq key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    char *payload = NULL;
    esp_err_t err = build_payload_json(text, &payload);
    if (err != ESP_OK) {
        return err;
    }

    bin_buf_t body;
    err = bin_buf_init(&body, 8192);
    if (err != ESP_OK) {
        free(payload);
        return err;
    }

    int status = 0;
    if (http_proxy_is_enabled()) {
        err = groq_http_proxy(payload, &body, &status);
    } else {
        err = groq_http_direct(payload, &body, &status);
    }
    free(payload);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Groq TTS request failed: %s", esp_err_to_name(err));
        bin_buf_free(&body);
        return err;
    }

    if (status != 200) {
        size_t preview_len = body.len < 160 ? body.len : 160;
        char preview[161] = {0};
        memcpy(preview, body.data, preview_len);
        for (size_t i = 0; i < preview_len; i++) {
            if (preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t') {
                preview[i] = ' ';
            }
        }
        ESP_LOGE(TAG, "Groq TTS HTTP %d: %s", status, preview);
        bin_buf_free(&body);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    if (!wav_header_seems_valid(body.data, body.len)) {
        ESP_LOGE(TAG, "Groq TTS returned non-WAV payload (%u bytes)", (unsigned)body.len);
        bin_buf_free(&body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_wav = body.data;
    *out_len = body.len;
    ESP_LOGI(TAG, "Groq TTS OK: %u bytes WAV", (unsigned)body.len);
    return ESP_OK;
}
