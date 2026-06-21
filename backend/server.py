"""GLM-Realtime-compatible WebSocket server (local Mac backend).

Speaks the subset of the GLM-Realtime protocol the device uses, so the firmware
connects unchanged (just point its backend_url at this server). Server VAD
(energy-based) detects end-of-utterance, then runs the STT->LLM->TTS pipeline
and streams response.audio.delta back at 24kHz.
"""
import asyncio
import json
import time
import uuid

import numpy as np
import websockets

import config
import pipeline
from audio_utils import b64_to_pcm16, pcm16_to_b64, rms


def evt(type_, **kw):
    return json.dumps({"type": type_, "event_id": uuid.uuid4().hex,
                       "client_timestamp": int(time.time() * 1000), **kw})


async def run_pipeline(ws, utterance: np.ndarray):
    loop = asyncio.get_event_loop()
    user_text, reply_text, audio24k = await loop.run_in_executor(None, pipeline.respond, utterance)
    print(f"  STT: {user_text!r}\n  LLM: {reply_text!r}\n  TTS: {len(audio24k)} samples @24k")
    await ws.send(evt("conversation.item.input_audio_transcription.completed", transcript=user_text))
    await ws.send(evt("response.created"))
    chunk = config.OUT_RATE * 40 // 1000  # ~40ms
    for i in range(0, len(audio24k), chunk):
        await ws.send(evt("response.audio.delta", delta=pcm16_to_b64(audio24k[i:i + chunk])))
        await asyncio.sleep(0.005)
    await ws.send(evt("response.audio.done"))
    await ws.send(evt("response.done", response={"status": "completed"}))


async def handler(ws):
    peer = getattr(ws, "remote_address", "?")
    print(f"[+] client connected: {peer}")
    await ws.send(evt("session.created", session={"model": "local-mlx", "modalities": ["text", "audio"]}))

    buf = []
    had_speech = False
    silence_ms = 0
    try:
        async for msg in ws:
            try:
                m = json.loads(msg)
            except Exception:
                continue
            t = m.get("type")
            if t == "session.update":
                await ws.send(evt("session.updated", session=m.get("session", {})))
            elif t == "input_audio_buffer.append":
                pcm = b64_to_pcm16(m.get("audio", ""))
                if pcm.size == 0:
                    continue
                frame_ms = pcm.size * 1000 // config.IN_RATE
                if rms(pcm) >= config.VAD_ENERGY_THRESH:
                    if not had_speech:
                        had_speech = True
                        await ws.send(evt("input_audio_buffer.speech_started"))
                    buf.append(pcm)
                    silence_ms = 0
                elif had_speech:
                    buf.append(pcm)
                    silence_ms += frame_ms
                    if silence_ms >= config.VAD_SILENCE_MS:
                        await ws.send(evt("input_audio_buffer.speech_stopped"))
                        await run_pipeline(ws, np.concatenate(buf))
                        buf, had_speech, silence_ms = [], False, 0
            elif t == "input_audio_buffer.commit":
                if buf:
                    await run_pipeline(ws, np.concatenate(buf))
                    buf, had_speech, silence_ms = [], False, 0
    except websockets.ConnectionClosed:
        pass
    print(f"[-] client disconnected: {peer}")


async def main():
    print(f"小葡萄 local backend listening on ws://{config.WS_HOST}:{config.WS_PORT}")
    async with websockets.serve(handler, config.WS_HOST, config.WS_PORT, max_size=8 * 1024 * 1024):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
