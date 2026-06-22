import os
import re
import asyncio
from pathlib import Path
from datetime import datetime

from fastapi import FastAPI, Request, Query, HTTPException
from openai import OpenAI

app = FastAPI()

UPLOAD_DIR = Path("uploads")
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)

SILICONFLOW_API_KEY = os.environ.get("SILICONFLOW_API_KEY")
if not SILICONFLOW_API_KEY:
    raise RuntimeError("Please set SILICONFLOW_API_KEY environment variable")

client = OpenAI(
    api_key=SILICONFLOW_API_KEY,
    base_url="https://api.siliconflow.cn/v1",
)

ASR_MODEL = "FunAudioLLM/SenseVoiceSmall"
MAX_FILE_SIZE = 50 * 1024 * 1024  # 50 MB


def safe_filename(filename: str | None) -> str:
    if not filename:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        return f"REC_{timestamp}.wav"

    name = Path(filename).name

    if not name or name in {".", ".."}:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        return f"REC_{timestamp}.wav"

    if not name.lower().endswith(".wav"):
        name += ".wav"

    return name


def avoid_overwrite(path: Path) -> Path:
    if not path.exists():
        return path

    stem = path.stem
    suffix = path.suffix

    for i in range(1, 10000):
        new_path = path.with_name(f"{stem}_{i}{suffix}")
        if not new_path.exists():
            return new_path

    raise RuntimeError("Too many duplicate filenames")


def transcribe_audio(audio_path: Path) -> str:
    with open(audio_path, "rb") as f:
        result = client.audio.transcriptions.create(
            model=ASR_MODEL,
            file=f,
        )

    text = re.sub(r"<\|[^|]+\|>", "", result.text).strip()

    txt_path = audio_path.with_suffix(".txt")
    txt_path.write_text(text, encoding="utf-8")

    print(f"Transcribed: {audio_path.name} -> {text!r}")

    return text


@app.post("/upload")
async def upload_audio(
    request: Request,
    filename: str | None = Query(default=None),
):
    data = await request.body()

    if not data:
        raise HTTPException(status_code=400, detail="Empty audio body")

    if len(data) > MAX_FILE_SIZE:
        raise HTTPException(status_code=400, detail="Audio file too large")

    name = safe_filename(filename)
    save_path = avoid_overwrite(UPLOAD_DIR / name)

    content_length = request.headers.get("content-length")
    if content_length and int(content_length) != len(data):
        print(f"WARNING: content-length={content_length} but received={len(data)} bytes (truncated!)")

    save_path.write_bytes(data)

    print(f"Saved audio: {save_path} ({len(data)} bytes, content-length={content_length})")

    try:
        text = await asyncio.to_thread(transcribe_audio, save_path)
    except Exception as exc:
        raise HTTPException(
            status_code=502,
            detail=f"Transcription failed: {exc}",
        ) from exc

    return {
        "saved": str(save_path),
        "bytes": len(data),
        "text": text,
    }


@app.get("/")
async def root():
    return {
        "message": "ASR server is running",
        "upload_url": "/upload",
        "upload_dir": str(UPLOAD_DIR),
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "server:app",
        host="0.0.0.0",
        port=8080,
        reload=True,
    )