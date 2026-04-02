#include "channels/voice/tts_client.h"

#if MIMI_VOICE_ENABLED

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "proxy/http_proxy.h"

#ifndef MIMI_SECRET_TTS_API_URL
#define MIMI_SECRET_TTS_API_URL ""
#endif

#ifndef MIMI_SECRET_TTS_KEY
#define MIMI_SECRET_TTS_KEY ""
#endif

#ifndef MIMI_SECRET_TTS_VOICE
#define MIMI_SECRET_TTS_VOICE "alloy"
#endif

#ifndef MIMI_TTS_TIMEOUT_MS
#ifdef MIMI_VOICE_TTS_TIMEOUT_MS
#define MIMI_TTS_TIMEOUT_MS MIMI_VOICE_TTS_TIMEOUT_MS
#else
#define MIMI_TTS_TIMEOUT_MS 15000
#endif
#endif

#define TTS_DEFAULT_API_URL              "https://api.openai.com/v1/audio/speech"
#define TTS_DEFAULT_MODEL                "tts-1"
#define TTS_DEFAULT_VOICE                "alloy"
#define TTS_SAMPLE_RATE_HZ               24000
#define TTS_URL_MAX_LEN                  256
#define TTS_KEY_MAX_LEN                  384
#define TTS_VOICE_MAX_LEN                32
#define TTS_RESPONSE_INITIAL_CAP         8192
#define TTS_MAX_RESPONSE_BYTES           (600 * 1024)
#define TTS_PROXY_READ_CHUNK             2048

#define VOICE_NVS_NAMESPACE              "voice_config"
#define VOICE_NVS_KEY_TTS_URL            "tts_url"
#define TTS_NVS_KEY_API_URL              "tts_api_url"  /* legacy in llm_config */
#define TTS_NVS_KEY_API_KEY              "tts_api_key"
#define TTS_NVS_KEY_VOICE                "tts_voice"

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} resp_buf_t;

typedef struct {
    bool https;
    char host[128];
    int port;
    char path[192];
} parsed_url_t;

static const char *TAG = "tts_client";

static bool s_initialized = false;
static char s_api_url[TTS_URL_MAX_LEN] = TTS_DEFAULT_API_URL;
static char s_api_key[TTS_KEY_MAX_LEN] = "";
static char s_voice[TTS_VOICE_MAX_LEN] = TTS_DEFAULT_VOICE;

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
    rb->data = heap_caps_malloc(initial_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rb->data) {
        return ESP_ERR_NO_MEM;
    }
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) {
        return ESP_OK;
    }

    if (rb->len + len > TTS_MAX_RESPONSE_BYTES) {
        return ESP_ERR_NO_MEM;
    }

    while (rb->len + len > rb->cap) {
        size_t new_cap = rb->cap * 2;
        if (new_cap > TTS_MAX_RESPONSE_BYTES) {
            new_cap = TTS_MAX_RESPONSE_BYTES;
        }
        if (new_cap < rb->len + len) {
            return ESP_ERR_NO_MEM;
        }

        uint8_t *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        rb->data = tmp;
        rb->cap = new_cap;
    }

    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
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

static size_t find_bytes(const uint8_t *buf, size_t buf_len, const char *needle, size_t needle_len)
{
    if (!buf || !needle || needle_len == 0 || buf_len < needle_len) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i + needle_len <= buf_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
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

static bool line_starts_with_ci(const char *line, size_t line_len, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    if (line_len < prefix_len) {
        return false;
    }

    for (size_t i = 0; i < prefix_len; i++) {
        char a = line[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool line_contains_ci(const char *line, size_t line_len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (line_len < nlen) {
        return false;
    }

    for (size_t i = 0; i + nlen <= line_len; i++) {
        bool ok = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = line[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + ('a' - 'A'));
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b + ('a' - 'A'));
            }
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }

    return false;
}

static esp_err_t decode_chunked_body(const uint8_t *body, size_t body_len, uint8_t **out_data, size_t *out_len)
{
    if (!body || !out_data || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    resp_buf_t out = {0};
    esp_err_t err = resp_buf_init(&out, body_len > 1024 ? body_len : 1024);
    if (err != ESP_OK) {
        return err;
    }

    size_t pos = 0;
    while (pos < body_len) {
        size_t line_rel = find_bytes(body + pos, body_len - pos, "\r\n", 2);
        if (line_rel == SIZE_MAX || line_rel == 0 || line_rel > 16) {
            resp_buf_free(&out);
            return ESP_ERR_INVALID_RESPONSE;
        }

        char size_str[20] = {0};
        memcpy(size_str, body + pos, line_rel);
        unsigned long chunk_size = strtoul(size_str, NULL, 16);

        pos += line_rel + 2;
        if (chunk_size == 0) {
            break;
        }
        if (pos + chunk_size > body_len) {
            resp_buf_free(&out);
            return ESP_ERR_INVALID_SIZE;
        }

        err = resp_buf_append(&out, body + pos, chunk_size);
        if (err != ESP_OK) {
            resp_buf_free(&out);
            return err;
        }

        pos += chunk_size;
        if (pos + 2 > body_len || body[pos] != '\r' || body[pos + 1] != '\n') {
            resp_buf_free(&out);
            return ESP_ERR_INVALID_RESPONSE;
        }
        pos += 2;
    }

    *out_data = out.data;
    *out_len = out.len;
    return ESP_OK;
}

static esp_err_t parse_proxy_http_response(resp_buf_t *rb, int *out_status)
{
    if (!rb || !rb->data || rb->len == 0 || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t hdr_end = find_bytes(rb->data, rb->len, "\r\n\r\n", 4);
    if (hdr_end == SIZE_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t line_end = find_bytes(rb->data, hdr_end, "\r\n", 2);
    if (line_end == SIZE_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char status_line[64] = {0};
    size_t status_line_len = line_end < sizeof(status_line) - 1 ? line_end : sizeof(status_line) - 1;
    memcpy(status_line, rb->data, status_line_len);
    const char *sp = strchr(status_line, ' ');
    *out_status = sp ? atoi(sp + 1) : 0;

    bool chunked = false;
    size_t content_length = 0;

    size_t pos = line_end + 2;
    while (pos < hdr_end) {
        size_t rel = find_bytes(rb->data + pos, hdr_end - pos, "\r\n", 2);
        if (rel == SIZE_MAX) {
            break;
        }
        size_t llen = rel;
        const char *line = (const char *)(rb->data + pos);

        if (line_starts_with_ci(line, llen, "transfer-encoding:")) {
            if (line_contains_ci(line, llen, "chunked")) {
                chunked = true;
            }
        } else if (line_starts_with_ci(line, llen, "content-length:")) {
            char tmp[32] = {0};
            size_t n = llen < sizeof(tmp) - 1 ? llen : sizeof(tmp) - 1;
            memcpy(tmp, line, n);
            const char *colon = strchr(tmp, ':');
            if (colon) {
                content_length = (size_t)strtoul(colon + 1, NULL, 10);
            }
        }

        pos += rel + 2;
    }

    const uint8_t *body = rb->data + hdr_end + 4;
    size_t body_len = rb->len - (hdr_end + 4);
    uint8_t *final_data = NULL;
    size_t final_len = 0;

    esp_err_t err = ESP_OK;
    if (chunked) {
        err = decode_chunked_body(body, body_len, &final_data, &final_len);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        final_len = (content_length > 0 && content_length <= body_len) ? content_length : body_len;
        final_data = heap_caps_malloc(final_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!final_data) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(final_data, body, final_len);
    }

    free(rb->data);
    rb->data = final_data;
    rb->len = final_len;
    rb->cap = final_len;
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return resp_buf_append(rb, (const uint8_t *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t tts_http_direct(const char *json_body, size_t json_len, resp_buf_t *rb, int *out_status)
{
    esp_http_client_config_t cfg = {
        .url = s_api_url,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = MIMI_TTS_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (s_api_key[0] != '\0') {
        char auth[TTS_KEY_MAX_LEN + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_http_client_set_post_field(client, json_body, (int)json_len);

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t tts_http_proxy(const char *json_body, size_t json_len, resp_buf_t *rb, int *out_status)
{
    parsed_url_t parsed = {0};
    if (!parse_url(s_api_url, &parsed) || !parsed.https) {
        ESP_LOGE(TAG, "Proxy TTS requires https URL, got: %s", s_api_url);
        return ESP_ERR_INVALID_ARG;
    }

    proxy_conn_t *conn = proxy_conn_open(parsed.host, parsed.port, MIMI_TTS_TIMEOUT_MS);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[1024];
    int header_len = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Authorization: Bearer %s\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n\r\n",
                              parsed.path,
                              parsed.host,
                              s_api_key,
                              (int)json_len);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        proxy_conn_close(conn);
        return ESP_ERR_INVALID_SIZE;
    }

    if (proxy_conn_write(conn, header, header_len) < 0 ||
        proxy_conn_write(conn, json_body, (int)json_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    uint8_t tmp[TTS_PROXY_READ_CHUNK];
    while (1) {
        int n = proxy_conn_read(conn, (char *)tmp, sizeof(tmp), MIMI_TTS_TIMEOUT_MS);
        if (n <= 0) {
            break;
        }
        esp_err_t err = resp_buf_append(rb, tmp, (size_t)n);
        if (err != ESP_OK) {
            proxy_conn_close(conn);
            return err;
        }
    }
    proxy_conn_close(conn);

    return parse_proxy_http_response(rb, out_status);
}

static esp_err_t tts_http_call(const char *json_body, size_t json_len, resp_buf_t *rb, int *out_status)
{
    if (http_proxy_is_enabled()) {
        return tts_http_proxy(json_body, json_len, rb, out_status);
    }
    return tts_http_direct(json_body, json_len, rb, out_status);
}

esp_err_t tts_client_init(void)
{
    safe_copy(s_api_url, sizeof(s_api_url), TTS_DEFAULT_API_URL);
    safe_copy(s_voice, sizeof(s_voice), TTS_DEFAULT_VOICE);
    s_api_key[0] = '\0';

    if (MIMI_SECRET_TTS_API_URL[0] != '\0') {
        safe_copy(s_api_url, sizeof(s_api_url), MIMI_SECRET_TTS_API_URL);
    }

    if (MIMI_SECRET_TTS_VOICE[0] != '\0') {
        safe_copy(s_voice, sizeof(s_voice), MIMI_SECRET_TTS_VOICE);
    }

    if (MIMI_SECRET_TTS_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_TTS_KEY);
    } else if (MIMI_SECRET_API_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_API_KEY);
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_url[TTS_URL_MAX_LEN] = {0};
        size_t len = sizeof(tmp_url);
        if (nvs_get_str(nvs, TTS_NVS_KEY_API_URL, tmp_url, &len) == ESP_OK && tmp_url[0]) {
            safe_copy(s_api_url, sizeof(s_api_url), tmp_url);
        }

        char tmp_key[TTS_KEY_MAX_LEN] = {0};
        len = sizeof(tmp_key);
        if (nvs_get_str(nvs, TTS_NVS_KEY_API_KEY, tmp_key, &len) == ESP_OK && tmp_key[0]) {
            safe_copy(s_api_key, sizeof(s_api_key), tmp_key);
        } else {
            len = sizeof(tmp_key);
            if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp_key, &len) == ESP_OK && tmp_key[0]) {
                safe_copy(s_api_key, sizeof(s_api_key), tmp_key);
            }
        }

        char tmp_voice[TTS_VOICE_MAX_LEN] = {0};
        len = sizeof(tmp_voice);
        if (nvs_get_str(nvs, TTS_NVS_KEY_VOICE, tmp_voice, &len) == ESP_OK && tmp_voice[0]) {
            safe_copy(s_voice, sizeof(s_voice), tmp_voice);
        }

        nvs_close(nvs);
    }

    if (nvs_open(VOICE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_url[TTS_URL_MAX_LEN] = {0};
        size_t len = sizeof(tmp_url);
        if (nvs_get_str(nvs, VOICE_NVS_KEY_TTS_URL, tmp_url, &len) == ESP_OK && tmp_url[0]) {
            safe_copy(s_api_url, sizeof(s_api_url), tmp_url);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0] == '\0') {
        ESP_LOGW(TAG, "TTS init: API key empty");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TTS client initialized (url=%s, voice=%s)", s_api_url, s_voice);
    return ESP_OK;
}

esp_err_t tts_synthesize(const char *text, tts_result_t *out_result)
{
    if (!text || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    out_result->pcm_data = NULL;
    out_result->pcm_len = 0;
    out_result->sample_rate = 0;

    if (text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        esp_err_t init_err = tts_client_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (s_api_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", TTS_DEFAULT_MODEL);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", s_voice);
    cJSON_AddStringToObject(root, "response_format", "pcm");

    char *json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_body) {
        return ESP_ERR_NO_MEM;
    }

    resp_buf_t rb = {0};
    esp_err_t err = resp_buf_init(&rb, TTS_RESPONSE_INITIAL_CAP);
    if (err != ESP_OK) {
        free(json_body);
        return err;
    }

    int http_status = 0;
    err = tts_http_call(json_body, strlen(json_body), &rb, &http_status);
    free(json_body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS request failed: %s", esp_err_to_name(err));
        resp_buf_free(&rb);
        return err;
    }

    if (http_status < 200 || http_status >= 300) {
        ESP_LOGE(TAG, "TTS HTTP status=%d", http_status);
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    if (rb.len == 0) {
        resp_buf_free(&rb);
        return ESP_ERR_NOT_FOUND;
    }

    out_result->pcm_data = rb.data;
    out_result->pcm_len = rb.len;
    out_result->sample_rate = TTS_SAMPLE_RATE_HZ;
    return ESP_OK;
}

void tts_result_free(tts_result_t *result)
{
    if (!result) {
        return;
    }

    free(result->pcm_data);
    result->pcm_data = NULL;
    result->pcm_len = 0;
    result->sample_rate = 0;
}

#endif
