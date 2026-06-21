# 小葡萄 — Mac 本地语音后端 (P2)

在 Mac（Apple Silicon）上跑一个 WebSocket 服务，**对设备伪装成 GLM-Realtime 协议**，内部用本地 **mlx-audio**（STT/TTS）+ **DeepSeek**（LLM）完成语音对话。设备固件无需改动——只要把 `backend_url` 指向这台 Mac 即可在「云(GLM) / 本地(Mac)」间切换。

```
设备 ──ws(GLM 协议子集)──> server.py ──VAD断句──> mlx STT ──> DeepSeek LLM ──> mlx TTS(24k) ──response.audio.delta──> 设备
```

## 环境

- Apple Silicon Mac（建议常开的 Mac mini）。
- Python 3.12（uv 自动管理；3.14 太新，mlx 未必适配）。

## 安装

```bash
cd backend
uv sync            # 装 websockets/numpy/openai/scipy/soundfile/mlx-audio
cp .env.example .env
# 编辑 .env，填 DEEPSEEK_API_KEY=<你的 key>
```

## 自检（装完先跑这个）

```bash
uv run python pipeline.py --llm        # 测 DeepSeek 连通（需 key）
uv run python pipeline.py --selftest   # TTS 合成"你好,我是小葡萄" -> selftest_tts.wav -> STT 回转
```
首次会从 HuggingFace 下载模型（whisper + Kokoro，数百 MB~GB）。

## 运行 + 本地联调

```bash
uv run python server.py                # 监听 ws://0.0.0.0:9000
# 另开一个终端，用模拟设备测全链路（协议骨架，不触发模型可改 pipeline 为 stub）：
uv run python test_client.py
```

## 让设备连本地后端

1. 查本机局域网 IP：`ipconfig getifaddr en0`（如 192.168.1.50）。
2. 设备 SoftAP 配网页里把 **backend_url** 填 `ws://192.168.1.50:9000`（注意：本地是 **ws://** 明文，不是 wss）。
   - 留空 = 默认走 GLM 云（`wss://open.bigmodel.cn/...`）。
3. 设备发的 `Authorization` 头本地服务端忽略；GLM key 仅云模式需要。

## 配置项（.env）

| 变量 | 默认 | 说明 |
|---|---|---|
| `DEEPSEEK_API_KEY` | — | 必填，DeepSeek LLM |
| `DEEPSEEK_MODEL` | deepseek-chat | |
| `WS_PORT` | 9000 | |
| `STT_MODEL` | whisper-large-v3-turbo | mlx-audio STT |
| `TTS_MODEL` | Kokoro-82M-bf16 | mlx-audio TTS |
| `TTS_LANG` / `TTS_VOICE` | z / zf_xiaobei | 中文女声 |
| `SYSTEM_PROMPT` | 小葡萄人格 | LLM 系统提示 |

## 文件

- `server.py` — WS 服务，GLM 协议子集 + 服务端能量 VAD
- `pipeline.py` — STT→LLM→TTS（懒加载模型 + 对话历史）
- `audio_utils.py` — base64 / 重采样 / RMS
- `config.py` — 配置（读 .env）
- `test_client.py` — 模拟设备，验证协议 + 下行链路

## 待办 / 已知

- STT/TTS 的 mlx-audio 调用需在装好 mlx 的机器上跑通自检（首次下载模型）；按需调 `TTS_VOICE`/模型。
- VAD 为能量阈值版，后续可换 silero（mlx_audio 自带 `vad`）。
- 音乐 Function Call、多轮对话策略属 P3。
