#pragma once

/* MimiClaw Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_PROXY_TYPE
#define MIMI_SECRET_PROXY_TYPE      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_ID
#define MIMI_SECRET_FEISHU_APP_ID   ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_SECRET
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#endif
#ifndef MIMI_SECRET_TAVILY_KEY
#define MIMI_SECRET_TAVILY_KEY      ""
#endif
#ifndef MIMI_SECRET_VOICE_WS_URL
#define MIMI_SECRET_VOICE_WS_URL    ""
#endif
#ifndef MIMI_SECRET_VOICE_WS_TOKEN
#define MIMI_SECRET_VOICE_WS_TOKEN  ""
#endif
#ifndef MIMI_SECRET_VOICE_WS_VERSION
#define MIMI_SECRET_VOICE_WS_VERSION "v1"
#endif
#ifndef MIMI_SECRET_GROQ_API_KEY
#define MIMI_SECRET_GROQ_API_KEY    ""
#endif
#ifndef MIMI_SECRET_TTS_MODEL
#define MIMI_SECRET_TTS_MODEL       "canopylabs/orpheus-v1-english"
#endif
#ifndef MIMI_SECRET_TTS_VOICE
#define MIMI_SECRET_TTS_VOICE       "austin"
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#define MIMI_TG_POLL_STACK           (12 * 1024)
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0
#define MIMI_TG_CARD_SHOW_MS         3000
#define MIMI_TG_CARD_BODY_SCALE      3

/* Feishu Bot */
#define MIMI_FEISHU_MAX_MSG_LEN          4096
#define MIMI_FEISHU_POLL_STACK           (12 * 1024)
#define MIMI_FEISHU_POLL_PRIO            5
#define MIMI_FEISHU_POLL_CORE            0
#define MIMI_FEISHU_WEBHOOK_PORT         18790
#define MIMI_FEISHU_WEBHOOK_PATH         "/feishu/events"
#define MIMI_FEISHU_WEBHOOK_MAX_BODY     (16 * 1024)

/* Agent Loop */
#define MIMI_AGENT_STACK             (24 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4
#define MIMI_AGENT_SEND_WORKING_STATUS 1

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define MIMI_LLM_PROVIDER_DEFAULT    "anthropic"
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_OPENAI_API_URL          "https://cliproxyapi.meganode.org/v1/chat/completions"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)
#define MIMI_LLM_LOG_VERBOSE_PAYLOAD 0
#define MIMI_LLM_LOG_PREVIEW_BYTES   160

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           16
#define MIMI_OUTBOUND_STACK          (12 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* TTS Service */
#define MIMI_TTS_QUEUE_LEN           6
#define MIMI_TTS_MAX_TEXT_LEN        360
#define MIMI_TTS_STACK               (10 * 1024)
#define MIMI_TTS_PRIO                5
#define MIMI_TTS_CORE                0
#define MIMI_TTS_HTTP_RETRY_COUNT   2
#define MIMI_TTS_HTTP_RETRY_BACKOFF_MS 250

/* Wake / Voice Session */
#define MIMI_WAKE_ENABLED_DEFAULT          1
#define MIMI_WAKE_COOLDOWN_MS              3500
#define MIMI_WAKE_LOCAL_DETECT_DEFAULT     0
#define MIMI_WAKE_RMS_THRESHOLD_DEFAULT    1800
#define MIMI_WAKE_RMS_CONSEC_FRAMES_DEFAULT 3

#define MIMI_VOICE_LISTEN_TIMEOUT_MS       7000
#define MIMI_VOICE_SESSION_MAX_AUDIO_BYTES (160 * 1024)
#define MIMI_VOICE_SESSION_STACK           (6 * 1024)
#define MIMI_VOICE_SESSION_PRIO            4
#define MIMI_VOICE_SESSION_CORE            0

/* Audio capture contract / HAL */
#define MIMI_AUDIO_FRAME_MAX_SAMPLES       320   /* 20 ms at 16k mono */
#define MIMI_MIC_CAPTURE_ENABLED_DEFAULT   0
#define MIMI_MIC_SAMPLE_RATE_HZ            16000
#define MIMI_MIC_FRAME_SAMPLES             320
#define MIMI_MIC_RING_DEPTH                24
#define MIMI_MIC_I2S_WS_GPIO               -1
#define MIMI_MIC_I2S_BCLK_GPIO             -1
#define MIMI_MIC_I2S_DIN_GPIO              -1
#define MIMI_AUDIO_CAPTURE_STACK           (5 * 1024)
#define MIMI_AUDIO_CAPTURE_PRIO            5
#define MIMI_AUDIO_CAPTURE_CORE            0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       MIMI_SPIFFS_BASE "/config"
#define MIMI_SPIFFS_MEMORY_DIR       MIMI_SPIFFS_BASE "/memory"
#define MIMI_SPIFFS_SESSION_DIR      MIMI_SPIFFS_BASE "/sessions"
#define MIMI_MEMORY_FILE             MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md"
#define MIMI_SOUL_FILE               MIMI_SPIFFS_CONFIG_DIR "/SOUL.md"
#define MIMI_USER_FILE               MIMI_SPIFFS_CONFIG_DIR "/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

/* Cron / Heartbeat */
#define MIMI_CRON_FILE               MIMI_SPIFFS_BASE "/cron.json"
#define MIMI_CRON_MAX_JOBS           16
#define MIMI_CRON_CHECK_INTERVAL_MS  (60 * 1000)
#define MIMI_HEARTBEAT_FILE          MIMI_SPIFFS_BASE "/HEARTBEAT.md"
#define MIMI_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* GPIO */
#define MIMI_GPIO_CONFIG_SECTION     1   /* enable GPIO tools */

/* Skills */
#define MIMI_SKILLS_PREFIX           MIMI_SPIFFS_BASE "/skills/"

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#define MIMI_WS_MAX_CLIENTS          4

/* Speaker MAX98357A (I2S TX) */
#define MIMI_SPK_I2S_WS_GPIO         16
#define MIMI_SPK_I2S_BCLK_GPIO       15
#define MIMI_SPK_I2S_DOUT_GPIO       17
#define MIMI_SPK_SD_GPIO             18  /* MAX98357A SD/EN pin */
#define MIMI_SPK_SD_ACTIVE_LEVEL     1
#define MIMI_SPK_PCM_ATTENUATION     1   /* 1=no attenuation, 2=-6dB, 4=-12dB */
#define MIMI_SPK_I2S_WRITE_TIMEOUT_MS 250

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_FEISHU              "feishu_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"
#define MIMI_NVS_VOICE               "voice_config"
#define MIMI_NVS_TTS                 "tts_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_FEISHU_APP_ID   "app_id"
#define MIMI_NVS_KEY_FEISHU_APP_SECRET "app_secret"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_TAVILY_KEY      "tavily_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
#define MIMI_NVS_KEY_PROXY_TYPE      "proxy_type"
#define MIMI_NVS_KEY_VOICE_WS_URL    "voice_ws_url"
#define MIMI_NVS_KEY_VOICE_WS_TOKEN  "voice_ws_token"
#define MIMI_NVS_KEY_VOICE_WS_VER      "voice_ws_ver"
#define MIMI_NVS_KEY_WAKE_ENABLED      "wake_enabled"
#define MIMI_NVS_KEY_WAKE_COOLDOWN_MS  "wake_cd_ms"
#define MIMI_NVS_KEY_WAKE_LOCAL_ENABLED "wake_local_en"
#define MIMI_NVS_KEY_WAKE_RMS_THRESHOLD "wake_rms_th"
#define MIMI_NVS_KEY_MIC_CAPTURE_ENABLED "mic_cap_en"
#define MIMI_NVS_KEY_GROQ_API_KEY      "groq_key"
#define MIMI_NVS_KEY_TTS_MODEL       "tts_model"
#define MIMI_NVS_KEY_TTS_VOICE       "tts_voice"

/* WiFi Onboarding (Captive Portal) */
#define MIMI_ONBOARD_AP_PREFIX    "MimiClaw-"
#define MIMI_ONBOARD_AP_PASS      ""          /* open network */
#define MIMI_ONBOARD_HTTP_PORT    80
#define MIMI_ONBOARD_DNS_STACK    (4 * 1024)
#define MIMI_ONBOARD_MAX_SCAN     20
