"""Backend configuration from environment (.env)."""
import os


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

# Audio
IN_RATE = 16000      # device uplink PCM16 16k mono
OUT_RATE = 24000     # device expects 24k downlink (it resamples 24k->16k)

# Server VAD (energy-based) — end-of-utterance detection
VAD_FRAME_MS = 20
VAD_SILENCE_MS = 600     # silence after speech => utterance end
VAD_ENERGY_THRESH = 500  # int16 RMS threshold for "speech"
