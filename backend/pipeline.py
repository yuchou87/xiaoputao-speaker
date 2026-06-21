"""Voice pipeline: STT (mlx-audio) -> LLM (DeepSeek) -> TTS (mlx-audio).

Models are lazy-loaded on first use (downloaded from HuggingFace once). Run a
local check on the Mac mini after `uv sync`:  `python pipeline.py --selftest`
(TTS a phrase -> STT it back -> print), and `--llm` to ping DeepSeek.
"""
import glob
import os
import sys
import tempfile
import uuid

import numpy as np
import soundfile as sf

import config
from audio_utils import resample_pcm16

# ---- lazy singletons -------------------------------------------------------
_stt_model = None
_tts_model = None
_deepseek = None
_history = []  # conversation history (single device)


def _get_stt():
    global _stt_model
    if _stt_model is None:
        from mlx_audio.stt.generate import load_model
        _stt_model = load_model(config.STT_MODEL)
    return _stt_model


def _get_tts():
    global _tts_model
    if _tts_model is None:
        from mlx_audio.tts.generate import load_model
        _tts_model = load_model(config.TTS_MODEL)
    return _tts_model


def _get_deepseek():
    global _deepseek
    if _deepseek is None:
        from openai import OpenAI
        _deepseek = OpenAI(api_key=config.DEEPSEEK_API_KEY, base_url=config.DEEPSEEK_BASE_URL)
    return _deepseek


# ---- STT -------------------------------------------------------------------
def transcribe(pcm16_16k: np.ndarray) -> str:
    from mlx_audio.stt.generate import generate_transcription
    with tempfile.TemporaryDirectory() as d:
        wav = os.path.join(d, "in.wav")
        sf.write(wav, pcm16_16k.astype(np.int16), config.IN_RATE, subtype="PCM_16")
        out_prefix = os.path.join(d, "out")
        res = generate_transcription(model=_get_stt(), audio=wav,
                                     output_path=out_prefix, format="txt", verbose=False)
        if isinstance(res, str) and res.strip():
            return res.strip()
        if res is not None and hasattr(res, "text"):
            return str(res.text).strip()
        txt = out_prefix + ".txt"
        if os.path.exists(txt):
            return open(txt, encoding="utf-8").read().strip()
    return ""


# ---- LLM (DeepSeek) --------------------------------------------------------
def chat(user_text: str) -> str:
    if not config.DEEPSEEK_API_KEY:
        return "（未配置 DeepSeek API key，无法回答）"
    _history.append({"role": "user", "content": user_text})
    msgs = [{"role": "system", "content": config.SYSTEM_PROMPT}] + _history[-12:]
    resp = _get_deepseek().chat.completions.create(
        model=config.DEEPSEEK_MODEL, messages=msgs, temperature=0.7, max_tokens=300)
    reply = resp.choices[0].message.content.strip()
    _history.append({"role": "assistant", "content": reply})
    return reply


def reset_history():
    _history.clear()


# ---- TTS -------------------------------------------------------------------
def synthesize(text: str) -> np.ndarray:
    """Return 24kHz PCM16 mono for `text`."""
    from mlx_audio.tts.generate import generate_audio
    with tempfile.TemporaryDirectory() as d:
        prefix = f"tts_{uuid.uuid4().hex[:8]}"
        generate_audio(text=text, model=_get_tts(), lang_code=config.TTS_LANG,
                       voice=config.TTS_VOICE, output_path=d, file_prefix=prefix,
                       audio_format="wav", join_audio=True, save=True, verbose=False)
        wavs = sorted(glob.glob(os.path.join(d, prefix + "*.wav")))
        if not wavs:
            raise RuntimeError("TTS produced no wav")
        pcm, sr = sf.read(wavs[0], dtype="int16")
        if pcm.ndim > 1:
            pcm = pcm[:, 0]
        return resample_pcm16(pcm, sr, config.OUT_RATE)


# ---- full turn -------------------------------------------------------------
def respond(utterance_16k: np.ndarray):
    user_text = transcribe(utterance_16k)
    reply_text = chat(user_text) if user_text else "（没听清，请再说一次）"
    audio_24k = synthesize(reply_text)
    return user_text, reply_text, audio_24k


# ---- local checks ----------------------------------------------------------
if __name__ == "__main__":
    if "--llm" in sys.argv:
        print("DeepSeek:", chat("用一句话讲个冷笑话"))
    if "--selftest" in sys.argv:
        print("TTS 合成中…")
        audio = synthesize("你好，我是小葡萄。")
        sf.write("selftest_tts.wav", audio, config.OUT_RATE, subtype="PCM_16")
        print(f"TTS ok: {audio.size} samples @24k -> selftest_tts.wav")
        in16 = resample_pcm16(audio, config.OUT_RATE, config.IN_RATE)
        print("STT 回转:", transcribe(in16))
