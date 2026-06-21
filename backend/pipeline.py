"""Voice pipeline: STT -> LLM -> TTS.

STAGE 1 (this commit): stubs so the WS protocol/path can be tested end-to-end
without models. transcribe/chat/synthesize are swapped for mlx-audio + DeepSeek
in later steps. `respond()` is the single entry the server calls.
"""
import numpy as np
import config


def transcribe(pcm16_16k: np.ndarray) -> str:
    """STT stub. Real impl: mlx-audio / mlx-whisper."""
    dur = len(pcm16_16k) / config.IN_RATE
    return f"[stub transcript, {dur:.1f}s of audio]"


def chat(user_text: str) -> str:
    """LLM stub. Real impl: DeepSeek API."""
    return "你好，我是小葡萄本地后端的测试回复。"


def synthesize(text: str) -> np.ndarray:
    """TTS stub -> 24k PCM16. Real impl: mlx-audio TTS. For now a 1s 440Hz tone."""
    n = int(config.OUT_RATE * 1.0)
    t = np.arange(n)
    return (3000 * np.sin(2 * np.pi * 440 * t / config.OUT_RATE)).astype(np.int16)


def respond(utterance_16k: np.ndarray):
    """Full turn. Returns (user_text, reply_text, audio_24k_pcm16)."""
    user_text = transcribe(utterance_16k)
    reply_text = chat(user_text)
    audio_24k = synthesize(reply_text)
    return user_text, reply_text, audio_24k
