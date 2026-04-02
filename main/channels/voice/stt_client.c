#include "channels/voice/stt_client.h"

#if MIMI_VOICE_ENABLED

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "proxy/http_proxy.h"

#ifndef MIMI_SECRET_STT_API_URL
#define MIMI_SECRET_STT_API_URL ""
#endif

#ifndef MIMI_SECRET_STT_KEY
#define MIMI_SECRET_STT_KEY ""
#endif

#ifndef MIMI_STT_TIMEOUT_MS
#ifdef MIMI_VOICE_STT_TIMEOUT_MS
#define MIMI_STT_TIMEOUT_MS MIMI_VOICE_STT_TIMEOUT_MS
#else
#define MIMI_STT_TIMEOUT_MS 15000
#endif
#endif

#define STT_DEFAULT_API_URL                 "https://api.openai.com/v1/audio/transcriptions"
#define STT_DEFAULT_MODEL                   "whisper-1"
#define STT_FORM_BOUNDARY                   "----mimiclawstt8f8cd57e"
#define STT_URL_MAX_LEN                     256
#define STT_KEY_MAX_LEN                     384
#define STT_RESPONSE_INITIAL_CAP            2048
#define STT_PROXY_READ_CHUNK                2048

#define VOICE_NVS_NAMESPACE                 "voice_config"
#define VOICE_NVS_KEY_STT_URL               "stt_url"
#define STT_NVS_KEY_API_URL                 "stt_api_url"  /* legacy in llm_config */
#define STT_NVS_KEY_API_KEY                 "stt_api_key"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

typedef struct {
    bool https;
    char host[128];
    int port;
    char path[192];
} parsed_url_t;

static const char *TAG = "stt_client";

static bool s_initialized = false;
static char s_api_url[STT_URL_MAX_LEN] = STT_DEFAULT_API_URL;
static char s_api_key[STT_KEY_MAX_LEN] = "";

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
    if (!rb->data) {
        return ESP_ERR_NO_MEM;
    }
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    if (!rb || !data || len == 0) {
        return ESP_OK;
    }

    while (rb->len + len + 1 > rb->cap) {
        size_t new_cap = rb->cap * 2;
        char *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        rb->data = tmp;
        rb->cap = new_cap;
    }

    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb)
{
    if (!rb) {
        return;
    }
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static void resp_buf_decode_chunked(resp_buf_t *rb)
{
    if (!rb || !rb->data || rb->len == 0) {
        return;
    }

    char *src = rb->data;
    char *dst = rb->data;
    char *end = rb->data + rb->len;

    while (src < end) {
        char *line_end = strstr(src, "\r\n");
        if (!line_end) {
            break;
        }

        unsigned long chunk_size = strtoul(src, NULL, 16);
        if (chunk_size == 0) {
            break;
        }

        src = line_end + 2;
        if (src + chunk_size > end) {
            break;
        }

        memmove(dst, src, chunk_size);
        dst += chunk_size;
        src += chunk_size;

        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n') {
            src += 2;
        }
    }

    rb->len = (size_t)(dst - rb->data);
    rb->data[rb->len] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return resp_buf_append(rb, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static bool parse_url(const char *url, parsed_url_t *out)
{
    if (!url || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const char *p = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        out->https = true;
        out->port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->https = false;
        out->port = 80;
        p = url + 7;
    } else {
        return false;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : (url + strlen(url));
    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return false;
    }

    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    char *colon = strchr(out->host, ':');
    if (colon) {
        *colon = '\0';
        out->port = atoi(colon + 1);
        if (out->port <= 0) {
            return false;
        }
    }

    if (slash) {
        safe_copy(out->path, sizeof(out->path), slash);
    } else {
        safe_copy(out->path, sizeof(out->path), "/");
    }

    return out->host[0] != '\0';
}

static void wav_write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void wav_write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint8_t *build_wav_from_pcm(const int16_t *pcm, size_t sample_count, size_t *out_wav_size)
{
    if (!pcm || !out_wav_size) {
        return NULL;
    }

    size_t pcm_bytes = sample_count * sizeof(int16_t);
    size_t wav_size = 44 + pcm_bytes;
    uint8_t *wav = heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        return NULL;
    }

    memcpy(wav + 0, "RIFF", 4);
    wav_write_u32_le(wav + 4, (uint32_t)(wav_size - 8));
    memcpy(wav + 8, "WAVE", 4);

    memcpy(wav + 12, "fmt ", 4);
    wav_write_u32_le(wav + 16, 16);
    wav_write_u16_le(wav + 20, 1);
    wav_write_u16_le(wav + 22, 1);
    wav_write_u32_le(wav + 24, 16000);
    wav_write_u32_le(wav + 28, 16000 * 1 * sizeof(int16_t));
    wav_write_u16_le(wav + 32, 1 * sizeof(int16_t));
    wav_write_u16_le(wav + 34, 16);

    memcpy(wav + 36, "data", 4);
    wav_write_u32_le(wav + 40, (uint32_t)pcm_bytes);

    memcpy(wav + 44, pcm, pcm_bytes);
    *out_wav_size = wav_size;
    return wav;
}

static char *build_multipart_body(const int16_t *pcm, size_t sample_count, size_t *out_body_len)
{
    if (!pcm || sample_count == 0 || !out_body_len) {
        return NULL;
    }

    size_t wav_len = 0;
    uint8_t *wav = build_wav_from_pcm(pcm, sample_count, &wav_len);
    if (!wav) {
        return NULL;
    }

    char prefix[512];
    int prefix_len = snprintf(prefix, sizeof(prefix),
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                              "%s\r\n"
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                              "Content-Type: audio/wav\r\n\r\n",
                              STT_FORM_BOUNDARY,
                              STT_DEFAULT_MODEL,
                              STT_FORM_BOUNDARY);
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(prefix)) {
        free(wav);
        return NULL;
    }

    char suffix[96];
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", STT_FORM_BOUNDARY);
    if (suffix_len <= 0 || suffix_len >= (int)sizeof(suffix)) {
        free(wav);
        return NULL;
    }

    size_t body_len = (size_t)prefix_len + wav_len + (size_t)suffix_len;
    char *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        free(wav);
        return NULL;
    }

    memcpy(body, prefix, (size_t)prefix_len);
    memcpy(body + prefix_len, wav, wav_len);
    memcpy(body + prefix_len + wav_len, suffix, (size_t)suffix_len);

    free(wav);
    *out_body_len = body_len;
    return body;
}

static esp_err_t stt_http_direct(const char *body, size_t body_len, resp_buf_t *rb, int *out_status)
{
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", STT_FORM_BOUNDARY);

    esp_http_client_config_t cfg = {
        .url = s_api_url,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = MIMI_STT_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);

    if (s_api_key[0] != '\0') {
        char auth[STT_KEY_MAX_LEN + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t stt_http_proxy(const char *body, size_t body_len, resp_buf_t *rb, int *out_status)
{
    parsed_url_t parsed = {0};
    if (!parse_url(s_api_url, &parsed) || !parsed.https) {
        ESP_LOGE(TAG, "Proxy STT requires https URL, got: %s", s_api_url);
        return ESP_ERR_INVALID_ARG;
    }

    proxy_conn_t *conn = proxy_conn_open(parsed.host, parsed.port, MIMI_STT_TIMEOUT_MS);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[1024];
    int header_len = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Authorization: Bearer %s\r\n"
                              "Content-Type: multipart/form-data; boundary=%s\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n\r\n",
                              parsed.path,
                              parsed.host,
                              s_api_key,
                              STT_FORM_BOUNDARY,
                              (int)body_len);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        proxy_conn_close(conn);
        return ESP_ERR_INVALID_SIZE;
    }

    if (proxy_conn_write(conn, header, header_len) < 0 ||
        proxy_conn_write(conn, body, (int)body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[STT_PROXY_READ_CHUNK];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), MIMI_STT_TIMEOUT_MS);
        if (n <= 0) {
            break;
        }
        esp_err_t append_err = resp_buf_append(rb, tmp, (size_t)n);
        if (append_err != ESP_OK) {
            proxy_conn_close(conn);
            return append_err;
        }
    }
    proxy_conn_close(conn);

    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) {
            *out_status = atoi(sp + 1);
        }
    }

    char *http_body = strstr(rb->data, "\r\n\r\n");
    if (http_body) {
        http_body += 4;
        size_t body_only_len = rb->len - (size_t)(http_body - rb->data);
        memmove(rb->data, http_body, body_only_len);
        rb->len = body_only_len;
        rb->data[rb->len] = '\0';
    }

    if (rb->len > 8 && strncmp(rb->data, "{", 1) != 0 && strstr(rb->data, "\r\n")) {
        resp_buf_decode_chunked(rb);
    }

    return ESP_OK;
}

static esp_err_t stt_http_call(const char *body, size_t body_len, resp_buf_t *rb, int *out_status)
{
    if (http_proxy_is_enabled()) {
        return stt_http_proxy(body, body_len, rb, out_status);
    }
    return stt_http_direct(body, body_len, rb, out_status);
}

esp_err_t stt_client_init(void)
{
    safe_copy(s_api_url, sizeof(s_api_url), STT_DEFAULT_API_URL);
    s_api_key[0] = '\0';

    if (MIMI_SECRET_STT_API_URL[0] != '\0') {
        safe_copy(s_api_url, sizeof(s_api_url), MIMI_SECRET_STT_API_URL);
    }

    if (MIMI_SECRET_STT_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_STT_KEY);
    } else if (MIMI_SECRET_API_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_API_KEY);
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_url[STT_URL_MAX_LEN] = {0};
        size_t len = sizeof(tmp_url);
        if (nvs_get_str(nvs, STT_NVS_KEY_API_URL, tmp_url, &len) == ESP_OK && tmp_url[0]) {
            safe_copy(s_api_url, sizeof(s_api_url), tmp_url);
        }

        char tmp_key[STT_KEY_MAX_LEN] = {0};
        len = sizeof(tmp_key);
        if (nvs_get_str(nvs, STT_NVS_KEY_API_KEY, tmp_key, &len) == ESP_OK && tmp_key[0]) {
            safe_copy(s_api_key, sizeof(s_api_key), tmp_key);
        } else {
            len = sizeof(tmp_key);
            if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp_key, &len) == ESP_OK && tmp_key[0]) {
                safe_copy(s_api_key, sizeof(s_api_key), tmp_key);
            }
        }

        nvs_close(nvs);
    }

    if (nvs_open(VOICE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_url[STT_URL_MAX_LEN] = {0};
        size_t len = sizeof(tmp_url);
        if (nvs_get_str(nvs, VOICE_NVS_KEY_STT_URL, tmp_url, &len) == ESP_OK && tmp_url[0]) {
            safe_copy(s_api_url, sizeof(s_api_url), tmp_url);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0] == '\0') {
        ESP_LOGW(TAG, "STT init: API key empty");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "STT client initialized (url=%s)", s_api_url);
    return ESP_OK;
}

esp_err_t stt_transcribe(const int16_t *pcm, size_t sample_count, char **out_text)
{
    if (out_text) {
        *out_text = NULL;
    }

    if (!pcm || sample_count == 0 || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        esp_err_t init_err = stt_client_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (s_api_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    size_t body_len = 0;
    char *body = build_multipart_body(pcm, sample_count, &body_len);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    resp_buf_t rb = {0};
    esp_err_t err = resp_buf_init(&rb, STT_RESPONSE_INITIAL_CAP);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    int http_status = 0;
    err = stt_http_call(body, body_len, &rb, &http_status);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT request failed: %s", esp_err_to_name(err));
        resp_buf_free(&rb);
        return err;
    }

    if (http_status < 200 || http_status >= 300) {
        ESP_LOGE(TAG, "STT HTTP status=%d body=%s", http_status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(rb.data);
    if (!root) {
        ESP_LOGE(TAG, "STT JSON parse error");
        resp_buf_free(&rb);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || text->valuestring[0] == '\0') {
        cJSON_Delete(root);
        resp_buf_free(&rb);
        return ESP_ERR_NOT_FOUND;
    }

    *out_text = strdup(text->valuestring);
    cJSON_Delete(root);
    resp_buf_free(&rb);

    if (!*out_text) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#endif
