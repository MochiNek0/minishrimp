# MiniShrimp: ~￥20 芯片上的口袋 AI 助理

<p align="center">
  <img src="assets/banner.png" alt="MiniShrimp" width="500" />
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>

> **声明：本项目是对 [mimiclaw](https://github.com/memovai/mimiclaw) 进行的二次开发。**

**~￥20 芯片上的 AI 助理。没有 Linux，没有 Node.js，纯 C。**

MiniShrimp 把一块小小的 ESP32-S3 开发板变成你的私人 AI 助理。插上 USB 供电，连上 WiFi，通过 **飞书 (Feishu)** 或 **Telegram** 与它对话 —— 它能处理你丢给它的任何任务，还会随时间积累本地记忆不断进化。一切全部跑在一颗拇指大小的芯片上。

## 核心特性

- **轻量级** —— 没有 Linux，没有 Node.js，也没有臃肿的依赖，纯 C 语言编写
- **多平台接入** —— 支持 **飞书机器人** 和 **Telegram Bot**，随心切换
- **国产大模型深度适配** —— 支持除了 Anthropic (Claude) 和 OpenAI (GPT) 以外的主流国产模型端点，如 Qwen, DeepSeek, Zhipu, Moonshot, Minimax, Yi, Doubao, Hunyuan, Baichuan, Qianfan, Spark 及各类自定义提供商
- **Web UI 实时配置** —— 运行时自带 Web Config UI 页面，可在浏览器中直接修改大模型密钥、WiFi 参数及代理配置，无需重编烧录
- **本地记忆** —— 从交互中学习，所有会话记忆与核心预设存放在本地 SPIFFS/NVS Flash，跨重启也不会忘
- **工具调用与定时任务** —— 支持 ReAct Agent 循环能力，可自主调用网页搜索、网页抓取、查看时间、文件读写等工具；内置 Cron 调度器，AI 可自主创建每日计划和提醒
- **极低功耗** —— USB 供电，24/7 运行仅约 0.5W，比一部手机的待机功耗还低

## 工作原理

你在飞书或 Telegram 发一条消息，ESP32-S3 通过 WiFi 收到后送进内部的 Agent 循环 — LLM 进行思考、规划、调用工具、读取本地记忆 — 再把最终回复返回给你。运行时可根据配置文件或 Web 页面自由设定所需的后端模型。

## 快速开始

### 你需要

- 一块 **ESP32-S3 开发板**，推荐 16MB Flash + 8MB PSRAM（例如小智 AI 开发板等）
- 一根 **USB Type-C 数据线**
- 一个 **飞书机器人 App ID 和 App Secret**（在飞书开发者后台创建）或 **Telegram Bot Token**
- 一个匹配对应大模型服务商的 **API Key**（如 OpenAI 兼容端点的 Key 等）

### 编译与安装

```bash
# 本项目基于 ESP-IDF v5.5+ 进行构建，请先安装配置好 IDF 环境:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/MochiNek0/minishrimp.git
cd minishrimp

idf.py set-target esp32s3
idf.py build
```

### 配置 (静态源码或动态 Web UI)

MiniShrimp 采取灵活的**两层配置**策略：运行时的 NVS 配置（通过 Web 页面修改）优先级高于源码编译时的默认值。

**源码静态配置 (作为初始化缺省值)**

```bash
cp main/shrimp_secrets.h.example main/shrimp_secrets.h
```

修改 `main/shrimp_secrets.h` 文件中的默认值：

```c
/* WiFi */
#define SHRIMP_SECRET_WIFI_SSID       "你的WiFi名称"
#define SHRIMP_SECRET_WIFI_PASS       "你的WiFi密码"

/* Telegram Bot 凭证 (选填，若使用 Telegram) */
#define SHRIMP_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl"

/* 飞书 Bot 凭证 (选填，若使用飞书) */
#define SHRIMP_SECRET_FEISHU_APP_ID   "cli_axxxxxxxxxx"
#define SHRIMP_SECRET_FEISHU_APP_SECRET "你的飞书Secret"

/* LLM 参数 */
#define SHRIMP_SECRET_API_KEY         "sk-xxxxxx"
#define SHRIMP_SECRET_MODEL           "deepseek-chat"  // 或其他模型名
#define SHRIMP_SECRET_MODEL_PROVIDER  "openai"         // 支持 anthropic/openai/qwen/deepseek/zhipu 等

/* 自定义 LLM 端点 (可选) */
#define SHRIMP_SECRET_CUSTOM_URL      ""  // 自定义 API 地址
#define SHRIMP_SECRET_CUSTOM_HEADER   ""  // 自定义认证头名称
#define SHRIMP_SECRET_CUSTOM_PREFIX   "Bearer "

/* HTTP 代理 (可选) */
#define SHRIMP_SECRET_PROXY_HOST      ""
#define SHRIMP_SECRET_PROXY_PORT      ""
#define SHRIMP_SECRET_PROXY_TYPE      ""   // "http" 或 "socks5"

/* 搜索 API (可选，用于 web_search 工具) */
#define SHRIMP_SECRET_TAVILY_KEY      ""   // Tavily 搜索 (推荐)
#define SHRIMP_SECRET_SEARCH_KEY      ""   // Brave 搜索
```

修改完成后，建议执行 `idf.py fullclean && idf.py build` 重编。

**Web UI 动态配置 (推荐)**
如果芯片内还没有网络配置，上电后它会产生一个 `MiniShrimp-Config` 的热点 (默认 IP: 192.168.4.1)，热点密码为 `shrimp1234`，连接后浏览器访问 `http://192.168.4.1:18789/config` 即可进入图形化网络配置页面。
如果已经接入了局域网，局域网内的设备访问 `http://<ESP32在局域网内的IP>:18789/config` 即可动态修改 API 密钥和代理设置。即改即生效。

### 烧录与日志查看

```bash
# 查找你的串口设备
ls /dev/cu.usb*          # macOS 设备
ls /dev/ttyACM*          # Linux / WSL

# 执行烧录（请将 PORT 替换为实际串口名称）
# 注意：大部分 ESP32-S3 把原生串口标为 USB 或 JTAG，请插到该接口而不是标有 COM 或 UART 的接口进行调试烧录！
idf.py -p PORT flash monitor
```

## 代理设置与内网穿透（国内网络环境）

如果仍在使用 Telegram Bot 或原生依赖海外 API 的模型端点，需要正确设置代理。MiniShrimp 原生支持 HTTP CONNECT 和 SOCKS5 透明隧道。

你可以预先在 `shrimp_secrets.h` 中进行如下设置，或直接通过局域网 **Web Config UI** 修改：

```c
#define SHRIMP_SECRET_PROXY_HOST      "192.168.31.10"
#define SHRIMP_SECRET_PROXY_PORT      "7890"
#define SHRIMP_SECRET_PROXY_TYPE      "http"  // 根据代理工具情况选择 "http" 或 "socks5"
```

> **提示**：确保被指定的局域网代理客户端（如 Clash 等）已经开启了“允许局域网连接”的开关。

## 本地记忆与 SPIFFS 文件系统

MiniShrimp 的关键资产数据保存在 SPIFFS 中，作为纯文本文件存放。如果需要自定义，可以在运行时进行覆盖：

| 文件           | 核心用途                                        |
| -------------- | ----------------------------------------------- |
| `SOUL.md`      | 设定机器人的人格和系统指令边界 (System Prompt)  |
| `USER.md`      | 保存你的基础身份信息和行为习惯特征              |
| `MEMORY.md`    | 由大模型自主追加的长期记忆与重要事件回顾        |
| `HEARTBEAT.md` | 定时任务或离线待办队列，每次心跳唤醒时检查      |
| `cron.json`    | 由 Agent 自行编排生成的下阶段 cron 调度配置清单 |

## 工具与技能支持

MiniShrimp 支持各类模型服务商进行符合 `OpenAI` 协议标准的函数调用 (Tool / Function Calling)。

| 工具名                                          | 用途                                                    |
| ----------------------------------------------- | ------------------------------------------------------- |
| `web_search`                                    | 通过 Tavily 或 Brave API 搜索互联网，回答时效性问题     |
| `web_fetch`                                     | 抓取网页内容并提取纯文本，支持代理和 Jina Reader 后备   |
| `get_current_time`                              | 请求获取当前的世界时间用于矫正内部 RTC 实钟             |
| `cron_add`/`list`/`remove`                      | 让 AI 自主设定闹钟、每日简报和备忘录循环提醒任务        |
| `read_file`/`write_file`/`edit_file`/`list_dir` | (对特定开发者开放) 直接检索、读写微控制器的内部文件系统 |

## 贡献者

感谢所有为本项目做出贡献的开发者！

[![Contributors](https://images.weserv.nl/?url=https://contrib.rocks/image?repo=MochiNek0/minishrimp)](https://github.com/MochiNek0/minishrimp/graphs/contributors)

## 许可证

本项目以 MIT 开源协议发布。

## 鸣谢

**本项目基于 [mimiclaw](https://github.com/memovai/mimiclaw) 进行二次开发**，感谢开源社区及原作者！
AI Agent 的实现灵感部分借助于 [OpenClaw](https://github.com/openclaw/openclaw) 与 [Nanobot](https://github.com/HKUDS/nanobot)。在 ~￥20 的微型单片机上将 ReAct 机制落地是一次激动人心的探索。
