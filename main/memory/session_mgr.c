#include "session_mgr.h"
#include "mimi_config.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";

static void sanitize_token(const char *src, char *dst, size_t dst_size, const char *fallback)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src || !src[0]) {
        snprintf(dst, dst_size, "%s", fallback);
        return;
    }

    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < dst_size - 1; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch) || ch == '_' || ch == '-') {
            dst[j++] = (char)ch;
        } else {
            dst[j++] = '_';
        }
    }
    dst[j] = '\0';

    if (j == 0) {
        snprintf(dst, dst_size, "%s", fallback);
    }
}

static void session_path(const char *channel, const char *chat_id, char *buf, size_t size)
{
    char ch[24];
    char cid[72];
    sanitize_token(channel, ch, sizeof(ch), MIMI_CHAN_SYSTEM);
    sanitize_token(chat_id, cid, sizeof(cid), "default");
    snprintf(buf, size, "%s/%s_%s.jsonl", MIMI_SPIFFS_SESSION_DIR, ch, cid);
}

static void session_path_legacy_tg(const char *chat_id, char *buf, size_t size)
{
    const char *cid = (chat_id && chat_id[0]) ? chat_id : "default";
    snprintf(buf, size, "%s/tg_%s.jsonl", MIMI_SPIFFS_SESSION_DIR, cid);
}

static FILE *session_open_read(const char *channel, const char *chat_id, char *path, size_t path_size)
{
    session_path(channel, chat_id, path, path_size);
    FILE *f = fopen(path, "r");
    if (f) {
        return f;
    }

    if (channel && strcmp(channel, MIMI_CHAN_TELEGRAM) == 0) {
        session_path_legacy_tg(chat_id, path, path_size);
        f = fopen(path, "r");
        if (f) {
            ESP_LOGI(TAG, "Using legacy Telegram session: %s", path);
        }
    }

    return f;
}

esp_err_t session_mgr_init(void)
{
    ESP_LOGI(TAG, "Session manager initialized at %s", MIMI_SPIFFS_SESSION_DIR);
    return ESP_OK;
}

esp_err_t session_append(const char *channel, const char *chat_id, const char *role, const char *content)
{
    if (!role || !content) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    session_path(channel, chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t session_get_history_json(const char *channel, const char *chat_id,
                                   char *buf, size_t size, int max_msgs)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int limit = max_msgs;
    if (limit <= 0 || limit > MIMI_SESSION_MAX_MSGS) {
        limit = MIMI_SESSION_MAX_MSGS;
    }

    char path[128];
    FILE *f = session_open_read(channel, chat_id, path, sizeof(path));
    if (!f) {
        snprintf(buf, size, "[]");
        return ESP_OK;
    }

    cJSON *messages[MIMI_SESSION_MAX_MSGS] = {0};
    int count = 0;
    int write_idx = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }

        cJSON *obj = cJSON_Parse(line);
        if (!obj) {
            continue;
        }

        if (count >= limit) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % limit;
        if (count < limit) {
            count++;
        }
    }
    fclose(f);

    cJSON *arr = cJSON_CreateArray();
    int start = (count < limit) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % limit;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    int cleanup_start = (count < limit) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % limit;
        cJSON_Delete(messages[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

esp_err_t session_clear(const char *channel, const char *chat_id)
{
    char path[128];
    session_path(channel, chat_id, path, sizeof(path));

    bool removed = (remove(path) == 0);

    if (channel && strcmp(channel, MIMI_CHAN_TELEGRAM) == 0) {
        char legacy_path[128];
        session_path_legacy_tg(chat_id, legacy_path, sizeof(legacy_path));
        removed = (remove(legacy_path) == 0) || removed;
    }

    if (removed) {
        ESP_LOGI(TAG, "Session cleared for %s:%s", channel ? channel : "(null)", chat_id ? chat_id : "(null)");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_SESSION_DIR);
    if (!dir) {
        dir = opendir(MIMI_SPIFFS_BASE);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open SPIFFS directory");
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len >= 6 && strcmp(name + len - 6, ".jsonl") == 0) {
            ESP_LOGI(TAG, "  Session: %s", name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
