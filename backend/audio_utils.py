"""Audio helpers: base64 PCM16, resample, RMS."""
import base64
import numpy as np
from scipy.signal import resample_poly


def b64_to_pcm16(b64: str) -> np.ndarray:
    raw = base64.b64decode(b64)
    return np.frombuffer(raw, dtype=np.int16)


def pcm16_to_b64(pcm: np.ndarray) -> str:
    return base64.b64encode(pcm.astype("<i2").tobytes()).decode("ascii")


def resample_pcm16(pcm: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate:
        return pcm.astype(np.int16)
    # rational resample
    from math import gcd
    g = gcd(src_rate, dst_rate)
    up, down = dst_rate // g, src_rate // g
    out = resample_poly(pcm.astype(np.float32), up, down)
    return np.clip(out, -32768, 32767).astype(np.int16)


def rms(pcm: np.ndarray) -> float:
    if pcm.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(pcm.astype(np.float32) ** 2)))
