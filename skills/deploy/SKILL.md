---
name: deploy
description: Deploy MiniShrimp firmware to an ESP32-S3 board. Covers prerequisites, configuration, build, flash, verification, and troubleshooting.
---

# Deploy MiniShrimp

End-to-end guide for deploying MiniShrimp to an ESP32-S3 dev board.

## Prerequisites

### Hardware

- ESP32-S3 dev board with **16 MB flash + 8 MB PSRAM** (e.g. Xiaozhi AI board, ~$5-10)
- USB Type-C data cable (not charge-only)

### Software

- **ESP-IDF v5.5+** installed and working
  ```bash
  # Install: https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/
  # Verify:
  idf.py --version   # should show >= 5.5
  ```

### Credentials (get these first)

- **WiFi SSID + password** — the network the ESP32 will connect to
- **Telegram Bot Token** — create via [@BotFather](https://t.me/BotFather) on Telegram
- **Anthropic API Key** — from [console.anthropic.com](https://console.anthropic.com)
- _(Optional)_ Brave Search API key — from [brave.com/search/api](https://brave.com/search/api/)
- _(Optional)_ HTTP proxy host:port — if in China or restricted network

## Step 1: Clone and Set Target

```bash
git clone https://github.com/memovai/minishrimp.git
cd minishrimp
idf.py set-target esp32s3
```

## Step 2: Configure Secrets

```bash
cp main/shrimp_secrets.h.example main/shrimp_secrets.h
```

Edit `main/shrimp_secrets.h` — fill in ALL required fields:

```c
#define SHRIMP_SECRET_WIFI_SSID       "YourWiFiName"        // REQUIRED
#define SHRIMP_SECRET_WIFI_PASS       "YourWiFiPassword"     // REQUIRED
#define SHRIMP_SECRET_TG_TOKEN        "123456:ABC-DEF..."    // REQUIRED
#define SHRIMP_SECRET_API_KEY         "sk-ant-api03-..."     // REQUIRED
#define SHRIMP_SECRET_MODEL           ""                     // optional, defaults to claude-opus-4-5
#define SHRIMP_SECRET_SEARCH_KEY      ""                     // optional: Brave Search API key
#define SHRIMP_SECRET_PROXY_HOST      ""                     // optional: e.g. "192.168.1.83"
#define SHRIMP_SECRET_PROXY_PORT      ""                     // optional: e.g. "7897"
```

**Proxy setup (China users):**
If you need a proxy to reach Telegram/Anthropic APIs, set both `PROXY_HOST` and `PROXY_PORT`. The proxy machine must:

- Be on the same LAN as the ESP32
- Support HTTP CONNECT method (Clash, V2Ray, etc.)
- Have "Allow LAN connections" enabled

## Step 3: Build

```bash
idf.py fullclean && idf.py build
```

**IMPORTANT:** Always `fullclean` after changing `shrimp_secrets.h` — the secrets are compiled into the binary.

Expected output: `Project build complete. To flash, run: idf.py flash`

### Build Troubleshooting

| Error                            | Fix                                                                           |
| -------------------------------- | ----------------------------------------------------------------------------- |
| `shrimp_secrets.h: No such file` | Run `cp main/shrimp_secrets.h.example main/shrimp_secrets.h`                  |
| `esp_websocket_client not found` | Run `idf.py fullclean` then `idf.py build` (managed component auto-downloads) |
| `Toolchain not found`            | Re-run ESP-IDF `install.sh` and `source export.sh`                            |
| Build runs out of memory         | Close other apps, ESP-IDF build needs ~2GB RAM                                |

## Step 4: Find Serial Port

```bash
# macOS
ls /dev/cu.usb*

# Linux
ls /dev/ttyACM* /dev/ttyUSB*
```

Common ports:

- macOS USB-OTG: `/dev/cu.usbmodem1101` or `/dev/cu.usbmodem11401`
- Linux: `/dev/ttyACM0`

**If no port shows up:**

- Try a different USB cable (must be data cable, not charge-only)
- Try a different USB port
- Check if board has a power LED lit

## Step 5: Flash

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your actual port. Example:

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

The monitor shows boot logs. Look for:

```
I (xxx) shrimp: MiniShrimp - ESP32-S3 AI Agent
I (xxx) shrimp: PSRAM free: ~8000000 bytes
I (xxx) wifi: WiFi connected: 192.168.x.x
I (xxx) telegram: Telegram bot token loaded
I (xxx) shrimp: All services started!
```

**Exit monitor:** `Ctrl+]`

## Step 6: Verify

1. Open Telegram, find your bot (the one you created with BotFather)
2. Send: `Hello`
3. You should see "shrimp is working..." followed by a response
4. Send: `Search for latest news about ESP32` — tests web_search (if Brave key set)

## Post-Deploy: Runtime Configuration

Connect via serial (`idf.py -p PORT monitor`) and use CLI commands:

```
shrimp> heap_info                    # check memory
shrimp> restart                      # reboot
```

CLI settings are stored in NVS flash and take priority over build-time values.

## OTA Update (over WiFi)

After initial USB flash, future updates can be done over WiFi:

1. Build new firmware: `idf.py build`
2. Host the `.bin` file on a local HTTP server:
   ```bash
   cd build && python3 -m http.server 8080
   ```
3. Send to your bot on Telegram or use the OTA CLI command with the URL:
   ```
   http://YOUR_PC_IP:8080/minishrimp.bin
   ```

## Flash Layout

```
16 MB Flash:
├── 0x009000  NVS (24 KB) — runtime config
├── 0x020000  OTA_0 (2 MB) — active firmware
├── 0x220000  OTA_1 (2 MB) — update slot
├── 0x420000  SPIFFS (12 MB) — memory, sessions, config
└── 0xFF0000  Coredump (64 KB)
```

## Common Issues

| Symptom                | Cause                     | Fix                                                            |
| ---------------------- | ------------------------- | -------------------------------------------------------------- |
| No WiFi connection     | Wrong SSID/password       | Check `shrimp_secrets.h`, `idf.py fullclean && build && flash` |
| "No bot token"         | Empty TG token            | Set via `shrimp_secrets.h` or the Web Config UI                |
| Bot doesn't respond    | API key invalid           | Check key at console.anthropic.com, set via web UI             |
| "Markdown send failed" | Normal with Markdown mode | Non-critical, falls back to plain text                         |
| Proxy timeout          | Proxy not reachable       | Ensure same LAN, proxy allows LAN connections                  |
| SPIFFS mount failed    | First boot or corruption  | Normal on first boot (auto-formats)                            |
| Port busy/not found    | Wrong port or cable       | Try different USB port/cable, check `ls /dev/cu.usb*`          |
| Boot loop              | Firmware crash            | Flash via USB again, check serial logs for crash info          |
