# crumb

An ESP32-based wearable audio recorder with a speech-to-text backend.

## Repository layout

| Path | Description |
|------|-------------|
| `firmware/` | ESP-IDF firmware. `crumb_V1_esp_idf` is the current board; `crumb_V0_test` holds early I2S/codec bring-up sketches (ES7210 TDM, ES8311, PDM). |
| `hardware/` | Board design files (`crumb_v1`). |
| `software/` | Python backend: a FastAPI upload/ASR server plus transcription utilities and the vendored `rnnoise` denoiser. |
| `doc/` | Progress notes and slides. |

## Software setup

```bash
cd software
pip install fastapi uvicorn openai requests
```

API keys are read from environment variables (never commit them):

```bash
export SILICONFLOW_API_KEY=...   # used by server.py (SenseVoice ASR)
export DEEPGRAM_API_KEY=...      # used by transcribe_deepgram.py
```

Run the upload/transcription server:

```bash
uvicorn server:app --host 0.0.0.0 --port 8000
```

Transcribe recorded WAV files with Deepgram:

```bash
python transcribe_deepgram.py --all
```

## Firmware

Built with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/). From a board directory:

```bash
cd firmware/crumb_V1_esp_idf
idf.py build flash monitor
```

## License

No license specified yet.
