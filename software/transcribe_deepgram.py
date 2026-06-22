"""
Deepgram STT transcription for ESP32 recordings.
Uses Deepgram REST API directly (no SDK needed).

Usage:
    python transcribe_deepgram.py                        # interactive
    python transcribe_deepgram.py --all                  # all WAV files
    python transcribe_deepgram.py --file foo.wav         # specific file
    python transcribe_deepgram.py --file foo.wav --diarize
"""

import argparse
import json
import os
from pathlib import Path

import requests

UPLOADS_DIR = Path(__file__).parent / "uploads"
DEEPGRAM_API_KEY = os.environ.get("DEEPGRAM_API_KEY")
if not DEEPGRAM_API_KEY:
    raise RuntimeError("Please set DEEPGRAM_API_KEY environment variable")
DEEPGRAM_URL = "https://api.deepgram.com/v1/listen"


def transcribe(wav_path: Path, diarize: bool = False, language: str = "zh") -> dict:
    params = {
        "model":        "nova-3-general",
        "language":     language,
        "smart_format": "true",
        "punctuate":    "true",
        "diarize":      "true" if diarize else "false",
        "utterances":   "true" if diarize else "false",
    }

    headers = {
        "Authorization": f"Token {DEEPGRAM_API_KEY}",
        "Content-Type":  "audio/wav",
    }

    with open(wav_path, "rb") as f:
        audio_bytes = f.read()

    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    resp = requests.post(DEEPGRAM_URL, params=params, headers=headers,
                         data=audio_bytes, timeout=60, verify=False)
    resp.raise_for_status()
    result = resp.json()

    # Save full JSON response
    json_path = wav_path.with_suffix(".deepgram.json")
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2),
                         encoding="utf-8")

    # Detected language
    detected_lang = (result.get("results", {})
                          .get("channels", [{}])[0]
                          .get("detected_language", "unknown"))
    print(f"  detected language: {detected_lang}")

    # Save transcript
    txt_path = wav_path.with_suffix(".deepgram.txt")
    utterances = result.get("results", {}).get("utterances")

    if diarize and utterances:
        lines = [
            f"[{u['start']:.1f}s-{u['end']:.1f}s] Speaker {u['speaker']}: {u['transcript']}"
            for u in utterances
        ]
        txt_path.write_text("\n".join(lines), encoding="utf-8")
        for line in lines:
            print(f"  {line}")
    else:
        transcript = (result["results"]["channels"][0]
                            ["alternatives"][0]["transcript"])
        txt_path.write_text(transcript, encoding="utf-8")
        print(f"  {transcript}")

    print(f"  → {txt_path.name}")
    print(f"  → {json_path.name}")
    return result


def list_wavs() -> list[Path]:
    return sorted(p for p in UPLOADS_DIR.glob("*.wav")
                  if "_processed" not in p.name)


def interactive(diarize: bool):
    wavs = list_wavs()
    if not wavs:
        print("No WAV files found in uploads/")
        return

    print("\nAvailable recordings:")
    for i, p in enumerate(wavs):
        print(f"  [{i+1}] {p.name}  ({p.stat().st_size // 1024} KB)")

    print("\nEnter numbers (e.g. 1 3), 'a' for all, 'q' to quit:")
    choice = input("> ").strip().lower()

    if choice == "q":
        return
    selected = wavs if choice == "a" else [
        wavs[int(x) - 1] for x in choice.split() if x.isdigit()
    ]

    print()
    for wav in selected:
        print(f"Transcribing: {wav.name}")
        try:
            transcribe(wav, diarize=diarize)
        except requests.HTTPError as e:
            print(f"  [HTTP error] {e.response.status_code}: {e.response.text}")
        except Exception as e:
            print(f"  [error] {e}")
        print()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--all",     action="store_true", help="Process all WAV files")
    parser.add_argument("--file",    type=str,            help="Specific WAV filename")
    parser.add_argument("--diarize",  action="store_true", help="Enable speaker diarization")
    parser.add_argument("--language", type=str, default="zh", help="Language code (default: zh)")
    args = parser.parse_args()

    if args.file:
        path = UPLOADS_DIR / args.file
        if not path.exists():
            print(f"File not found: {path}")
            return
        print(f"Transcribing: {path.name}  [lang={args.language}]")
        try:
            transcribe(path, diarize=args.diarize, language=args.language)
        except requests.HTTPError as e:
            print(f"[HTTP error] {e.response.status_code}: {e.response.text}")
    elif args.all:
        for wav in list_wavs():
            print(f"Transcribing: {wav.name}  [lang={args.language}]")
            try:
                transcribe(wav, diarize=args.diarize, language=args.language)
            except Exception as e:
                print(f"  [error] {e}")
            print()
    else:
        interactive(diarize=args.diarize)


if __name__ == "__main__":
    main()
