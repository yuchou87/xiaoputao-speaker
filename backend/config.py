"""Backend configuration from environment (.env)."""
import os

# Model downloads use HuggingFace. We do NOT force an endpoint — control it
# yourself via env if needed:
#   export HF_ENDPOINT=https://hf-mirror.com   # CN mirror (if it has the model)
#   uv run hf auth login                       # or auth to real HF for rate limits


def _load_dotenv(path: str = ".env") -> None:
    if not os.path.exists(path):
        return
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())


_load_dotenv()

WS_HOST = os.environ.get("WS_HOST", "0.0.0.0")
WS_PORT = int(os.environ.get("WS_PORT", "9000"))

DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY", "")
DEEPSEEK_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")
DEEPSEEK_BASE_URL = os.environ.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com")

SYSTEM_PROMPT = os.environ.get("SYSTEM_PROMPT",
    "你是「小葡萄」，一个友好、风趣的中文语音助手，运行在用户家里的智能音箱上。"
    "回答简洁口语化，单次回复控制在100字内，适合语音播报。会讲故事和笑话。")

# mlx-audio models (auto-downloaded from HuggingFace on first use)
STT_MODEL = os.environ.get("STT_MODEL", "mlx-community/Qwen3-ASR-1.7B-8bit")
TTS_MODEL = os.environ.get("TTS_MODEL", "mlx-community/Qwen3-TTS-12Hz-0.6B-Base-bf16")
# TTS extras (Qwen3-TTS may differ from Kokoro; lang/voice often ignored, a
# "Base" voice-clone model may need a reference voice — verify via --selftest).
TTS_LANG = os.environ.get("TTS_LANG", "zh")
TTS_VOICE = os.environ.get("TTS_VOICE", "")        # empty = model default
TTS_REF_AUDIO = os.environ.get("TTS_REF_AUDIO", "")  # optional ref wav for voice clone
TTS_REF_TEXT = os.environ.get("TTS_REF_TEXT", "")    # transcript of ref wav

# Audio
IN_RATE = 16000      # device uplink PCM16 16k mono
OUT_RATE = 24000     # device expects 24k downlink (it resamples 24k->16k)

# Server VAD (energy-based) — end-of-utterance detection
VAD_FRAME_MS = 20
VAD_SILENCE_MS = 600     # silence after speech => utterance end
VAD_ENERGY_THRESH = 400  # int16 RMS threshold for "speech" (device applies ~12x uplink gain)
