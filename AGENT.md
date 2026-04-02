## Project Overview

MimiClaw is an embedded AI assistant firmware for the ESP32-S3 microcontroller (16MB flash, 8MB PSRAM). It is pure C running on FreeRTOS with no Linux/Node.js dependencies. The device connects via WiFi and acts as an AI agent accessible through Telegram, Feishu, WebSocket, and an onboard voice channel (wake-word → STT → agent → TTS).

## Build & Flash

**Prerequisites:** ESP-IDF v5.5+ (install via `./scripts/setup_idf_ubuntu.sh` on Ubuntu)

```bash
# First-time setup
cp main/mimi_secrets.h.example main/mimi_secrets.h
# Edit main/mimi_secrets.h with WiFi SSID/password and API keys

# Voice feature flag (compile-time)
# main/mimi_config.h: #define MIMI_VOICE_ENABLED 1
# Set to 0 to disable voice channel, then rebuild.

# Build
idf.py set-target esp32s3
idf.py fullclean && idf.py build   # fullclean required after mimi_secrets.h or feature-flag changes

# Flash and monitor
idf.py -p /dev/ttyACM0 flash monitor
```

CI runs automatically on push via `.github/workflows/build.yml` using `espressif/idf:v5.5.2`, and validates both firmware image and ESP-SR model artifact (`build/srmodels/srmodels.bin`).

## Configuration

All configuration is **build-time** via `main/mimi_secrets.h` (gitignored). There is no runtime config UI. The example template is `main/mimi_secrets.h.example`. Compile-time constants (task priorities, stack sizes, buffer sizes, feature flags) are in `main/mimi_config.h`.

Runtime config is also possible via the serial CLI (UART port, not USB-JTAG):
```
wifi_set <SSID> <password>
set_api_key <key>
set_model_provider anthropic|openai
set_model <model-id>
set_stt_url <url>
set_tts_url <url>
set_wake_word <model>
```

## Architecture

### Core Separation
- **Core 0**: Network I/O — Telegram long-polling, Feishu webhook (port 18790), WebSocket server (port 18789), Serial CLI REPL, outbound message dispatch, voice feed task
- **Core 1**: Agent loop — CPU-bound JSON parsing, LLM API calls, tool execution, memory operations, voice detect task

### Message Flow
```
[Telegram/Feishu/WebSocket/CLI/Voice(STT)]
    → inbound FreeRTOS queue
    → agent_loop
    → outbound FreeRTOS queue
    → dispatch (Telegram/Feishu/WebSocket/Voice TTS playback)
```

The central message type is `mimi_msg_t` defined in `main/bus/message_bus.h`.

### Voice Pipeline (`main/channels/voice/voice_channel.c`)
```
Mic (audio_hal)
  → voice_feed_task → voice_afe_feed
  → voice_detect_task (WakeNet + VAD)
  → wake word detected
  → play wake chime (880Hz ~200ms, WakeNet temporarily disabled)
  → recording buffer (PSRAM)
  → STT client
  → inbound queue / agent_loop
  → outbound queue (channel="voice")
  → TTS client + speaker playback
```

### Agent Loop (`main/agent/agent_loop.c`)
Implements a ReAct loop (max 10 iterations per message):
1. Load session history (JSONL from SPIFFS)
2. Build system prompt from SOUL.md + USER.md + MEMORY.md + recent daily notes
3. Call LLM API (`main/llm/llm_proxy.c`) — Anthropic or OpenAI, non-streaming
4. Parse JSON response for text blocks and `tool_use` blocks
5. If `tool_use`: execute tool via tool registry, append result, repeat loop
6. If `end_turn`: save messages to session file, push response to outbound queue

### SPIFFS Storage + Model Partition
| Path / Partition | Purpose |
|------|---------|
| `/spiffs/config/SOUL.md` | AI personality prompt |
| `/spiffs/config/USER.md` | User profile |
| `/spiffs/memory/MEMORY.md` | Long-term persistent memory |
| `/spiffs/memory/YYYY-MM-DD.md` | Daily notes (ring buffer) |
| `/spiffs/sessions/tg_*.jsonl` | Per-chat conversation history |
| `/spiffs/cron.json` | Persistent cron job definitions |
| `/spiffs/skills/*.md` | Skill plugin definitions |
| `model` partition (`0xD20000`, 0x2D0000) | ESP-SR speech model bundle (`srmodels.bin`) used by wake-word/AFE pipeline |

### Tool System (`main/tools/`)
Tools are registered in `tool_registry.c` with JSON schemas. Available tools: `web_search` (Brave/Tavily), `cron` (schedule tasks), `get_time` (NTP), `files` (SPIFFS read/write/list), `gpio` (pin control). The agent invokes tools by name via the LLM's tool_use response.

Voice processing is implemented as a channel pipeline (not an LLM tool), and runtime voice diagnostics/config are exposed via serial CLI commands (`voice_status`, `voice_test_mic`, `voice_test_spk`, `set_stt_url`, `set_tts_url`, `set_wake_word`).

### Key Module Map
```
main/
├── mimi.c              # app_main, startup sequence
├── mimi_config.h       # All compile-time constants
├── mimi_secrets.h      # Gitignored credentials
├── audio/              # I2S/audio HAL (mic + speaker)
├── bus/                # FreeRTOS queues (inbound + outbound)
├── wifi/               # STA mode WiFi manager
├── channels/           # telegram/, feishu/, voice/ input/output channels
├── llm/                # Anthropic/OpenAI API client
├── agent/              # ReAct loop + context/prompt builder
├── tools/              # Tool registry and implementations
├── memory/             # SOUL/USER/MEMORY.md + session JSONL
├── gateway/            # WebSocket server output
├── cli/                # Serial REPL (esp_console)
├── cron/               # Persistent cron scheduler
├── heartbeat/          # Periodic autonomous task check
├── skills/             # SKILL.md plugin loader
├── ota/                # OTA firmware update
├── proxy/              # HTTP CONNECT tunnel for restricted nets
└── onboard/            # WiFi AP for first-time device setup
```

## Memory Budget (ESP32-S3)
Critical constraint: 8 MB PSRAM, ~256 KB internal SRAM. TLS connections consume ~60 KB each from PSRAM. All large buffers (JSON parse, session cache, system prompt, LLM response) must be allocated from PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.

Voice channel adds extra runtime memory pressure:
- Recording buffer in PSRAM: `MIMI_VOICE_MAX_RECORD_MS` (default 10s @16k mono int16) ≈ **320 KB**
- TTS response/audio buffers can reach hundreds of KB (implementation cap ~600 KB)
- STT multipart/HTTP response buffers also allocate from PSRAM
- Internal SRAM usage rises due to voice tasks (`MIMI_VOICE_FEED_STACK` + `MIMI_VOICE_DETECT_STACK`, default 8KB each) and small working buffers (feed frame + wake chime tone)

## No Unit Tests
There are no unit tests. Validation is done via GitHub Actions CI build + manual hardware testing. When making changes, build locally with `idf.py build` to verify compilation.
