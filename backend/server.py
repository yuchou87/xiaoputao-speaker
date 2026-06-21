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
    chunk = config.OUT_RATE * 40 // 1000  # ~40ms of audio per delta
    for i in range(0, len(audio24k), chunk):
        await ws.send(evt("response.audio.delta", delta=pcm16_to_b64(audio24k[i:i + chunk])))
        # Pace near real-time: the device's downlink ring buffer only holds a few
        # seconds, so bursting the whole reply overflows it and the tail is dropped
        # (playback cut short). Send a hair faster than playback to avoid underrun.
        await asyncio.sleep(0.036)
    await ws.send(evt("response.audio.done"))
    await ws.send(evt("response.done", response={"status": "completed"}))


async def handler(ws):
    peer = getattr(ws, "remote_address", "?")
    print(f"[+] client connected: {peer}")
    await ws.send(evt("session.created", session={"model": "local-mlx", "modalities": ["text", "audio"]}))

    buf = []
    had_speech = False
    silence_ms = 0
    busy = False   # a turn is being produced; keep draining the socket but drop audio

    async def process(utterance):
        nonlocal busy
        try:
            await run_pipeline(ws, utterance)
        except websockets.ConnectionClosed:
            pass
        except Exception as e:  # noqa: BLE001 - keep the connection alive on pipeline errors
            print(f"  pipeline error: {e}")
        finally:
            busy = False

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
                # End-of-utterance is decided on the DEVICE (its AFE VAD) and
                # signalled via input_audio_buffer.commit. Here we just buffer the
                # audio. Energy VAD on this side proved fragile (the device's
                # uplink gain pushed the noise floor above any fixed threshold),
                # so we no longer auto-detect speech_stopped from it.
                if busy:
                    continue   # drop audio while a reply is being produced
                pcm = b64_to_pcm16(m.get("audio", ""))
                if pcm.size == 0:
                    continue
                if not had_speech:
                    had_speech = True
                    await ws.send(evt("input_audio_buffer.speech_started"))
                buf.append(pcm)
                silence_ms += pcm.size * 1000 // config.IN_RATE
                # Safety fallback if a commit is ever lost: cap the utterance.
                if silence_ms >= 20000:
                    print("  [vad] 20s cap -> running pipeline")
                    utterance = np.concatenate(buf)
                    buf, had_speech, silence_ms = [], False, 0
                    busy = True
                    asyncio.create_task(process(utterance))
            elif t == "input_audio_buffer.commit":
                if buf and not busy:
                    print(f"  [vad] commit -> running pipeline ({silence_ms} ms audio)")
                    utterance = np.concatenate(buf)
                    buf, had_speech, silence_ms = [], False, 0
                    busy = True
                    asyncio.create_task(process(utterance))
    except websockets.ConnectionClosed:
        pass
    print(f"[-] client disconnected: {peer}")


async def main():
    print("preloading models (STT/TTS/LLM)...")
    await asyncio.get_event_loop().run_in_executor(None, pipeline.preload)
    print(f"小葡萄 local backend listening on ws://{config.WS_HOST}:{config.WS_PORT}")
    async with websockets.serve(handler, config.WS_HOST, config.WS_PORT, max_size=8 * 1024 * 1024):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
