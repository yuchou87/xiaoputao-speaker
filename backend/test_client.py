"""Simulated device: connect, send session.update, stream a fake utterance
(speech then silence), collect response.audio.delta, save reply to reply.wav.
Validates the WS protocol + downlink path without the real ESP32 board.
"""
import asyncio
import json
import sys

import numpy as np
import soundfile as sf
import websockets

from audio_utils import b64_to_pcm16, pcm16_to_b64, resample_pcm16
import config


def _build_utterance() -> np.ndarray:
    """Utterance to send. Pass a .wav arg for a REAL speech test (it's
    resampled to 16k); otherwise 1s noise (just exercises the protocol)."""
    wav = next((a for a in sys.argv[1:] if a.endswith(".wav")), None)
    if wav:
        pcm, sr = sf.read(wav, dtype="int16")
        if pcm.ndim > 1:
            pcm = pcm[:, 0]
        pcm = resample_pcm16(pcm, sr, config.IN_RATE)
        print(f"utterance from {wav}: {pcm.size} samples @16k")
        silence = np.zeros(int(config.IN_RATE * 0.8), dtype=np.int16)
        return np.concatenate([pcm, silence])
    rng = np.random.default_rng(0)
    speech = (rng.normal(0, 4000, config.IN_RATE).clip(-32768, 32767)).astype(np.int16)
    silence = np.zeros(int(config.IN_RATE * 0.8), dtype=np.int16)
    return np.concatenate([speech, silence])


async def main():
    uri = next((a for a in sys.argv[1:] if a.startswith("ws")), f"ws://127.0.0.1:{config.WS_PORT}")
    async with websockets.connect(uri, max_size=8 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"type": "session.update", "session": {
            "model": "glm-realtime", "input_audio_format": "pcm16",
            "output_audio_format": "pcm", "voice": "tongtong",
            "turn_detection": {"type": "server_vad"}}}))

        stream = _build_utterance()
        frame = config.IN_RATE * 20 // 1000  # 20ms
        for i in range(0, len(stream), frame):
            await ws.send(json.dumps({"type": "input_audio_buffer.append",
                                      "audio": pcm16_to_b64(stream[i:i + frame])}))
            await asyncio.sleep(0.005)

        reply = bytearray()
        got = {"created": False, "updated": False, "delta": 0, "done": False}
        try:
            async for msg in ws:
                m = json.loads(msg)
                t = m.get("type")
                if t == "session.created": got["created"] = True
                elif t == "session.updated": got["updated"] = True
                elif t == "conversation.item.input_audio_transcription.completed":
                    print("transcript:", m.get("transcript"))
                elif t == "response.audio.delta":
                    got["delta"] += 1
                    reply += b64_to_pcm16(m["delta"]).tobytes()
                elif t == "response.done":
                    got["done"] = True
                    break
        except websockets.ConnectionClosed:
            pass

        pcm = np.frombuffer(bytes(reply), dtype=np.int16)
        if pcm.size:
            sf.write("reply.wav", pcm, config.OUT_RATE, subtype="PCM_16")
        print("RESULT:", got, f"reply_samples={pcm.size} (-> reply.wav @24k)")
        ok = got["created"] and got["updated"] and got["delta"] > 0 and got["done"]
        print("PASS" if ok else "FAIL")


if __name__ == "__main__":
    asyncio.run(main())
