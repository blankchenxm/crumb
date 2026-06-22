"""
Audio post-processor for ESP32 recordings.
Usage:
    python process_audio.py                  # interactive selection
    python process_audio.py --all            # process all WAV files
    python process_audio.py --file foo.wav   # process specific file
"""

import argparse
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile

UPLOADS_DIR = Path(__file__).parent / "uploads"
HP_CUTOFF_HZ = 80
TARGET_PEAK_DBFS = -3
TARGET_PEAK_LINEAR = 10 ** (TARGET_PEAK_DBFS / 20)


def dbfs(x: float) -> str:
    if x < 1e-10:
        return "-inf dBFS"
    return f"{20 * np.log10(x):.1f} dBFS"


def process(wav_path: Path, overwrite: bool = False) -> Path:
    sr, data = wavfile.read(wav_path)
    print(f"  dtype={data.dtype}  shape={data.shape}  sr={sr} Hz")

    # Convert to float64
    if data.dtype == np.int16:
        f = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        f = data.astype(np.float64) / 2147483648.0
    elif data.dtype in (np.float32, np.float64):
        f = data.astype(np.float64)
    else:
        print(f"  [skip] unsupported dtype {data.dtype}")
        return wav_path

    peak_raw = np.max(np.abs(f))
    print(f"  peak (raw)          : {peak_raw:.6f}  {dbfs(peak_raw)}")

    # DC removal per channel
    if f.ndim == 2:
        for ch in range(f.shape[1]):
            f[:, ch] -= np.mean(f[:, ch])
    else:
        f -= np.mean(f)

    peak_dc = np.max(np.abs(f))
    print(f"  peak (after DC rm)  : {peak_dc:.6f}  {dbfs(peak_dc)}")

    # High-pass filter at HP_CUTOFF_HZ
    sos = signal.butter(4, HP_CUTOFF_HZ, btype="highpass", fs=sr, output="sos")
    if f.ndim == 2:
        for ch in range(f.shape[1]):
            f[:, ch] = signal.sosfilt(sos, f[:, ch])
    else:
        f = signal.sosfilt(sos, f)

    peak_hp = np.max(np.abs(f))
    rms_hp  = np.sqrt(np.mean(f ** 2))
    pct995  = np.percentile(np.abs(f), 99.5)   # ignore top 0.5% spikes
    print(f"  peak (after HP {HP_CUTOFF_HZ}Hz): {peak_hp:.6f}  {dbfs(peak_hp)}")
    print(f"  RMS  (after HP {HP_CUTOFF_HZ}Hz): {rms_hp:.6f}  {dbfs(rms_hp)}")
    print(f"  99.5 pct            : {pct995:.6f}  {dbfs(pct995)}")

    # Percentile-based normalization (ignores button-press spikes)
    ref = pct995 if pct995 > 1e-9 else peak_hp
    if ref < 1e-9:
        print("  [warn] signal too weak, skipping normalization")
    else:
        gain = TARGET_PEAK_LINEAR / ref
        f *= gain
        # Hard-clip any remaining spikes
        f = np.clip(f, -1.0, 1.0)
        print(f"  gain applied        : {gain:.1f}x  ({20*np.log10(gain):+.1f} dB)")
        print(f"  peak (final)        : {np.max(np.abs(f)):.6f}  {dbfs(np.max(np.abs(f)))}")

    out_data = np.clip(f * 32767, -32768, 32767).astype(np.int16)

    out_path = wav_path if overwrite else wav_path.with_stem(wav_path.stem.removesuffix("_processed") + "_processed")
    wavfile.write(out_path, sr, out_data)
    duration = out_data.shape[0] / sr
    print(f"  saved → {out_path.name}  ({duration:.1f}s)")
    return out_path


def list_wavs() -> list[Path]:
    return sorted(p for p in UPLOADS_DIR.glob("*.wav") if "_processed" not in p.name)


def interactive():
    wavs = list_wavs()
    if not wavs:
        print("No WAV files found in uploads/")
        return

    print("\nAvailable recordings:")
    for i, p in enumerate(wavs):
        size_kb = p.stat().st_size / 1024
        print(f"  [{i+1}] {p.name}  ({size_kb:.0f} KB)")

    print("\nEnter numbers (e.g. 1 3 5), or 'a' for all, or 'q' to quit:")
    choice = input("> ").strip().lower()

    if choice == "q":
        return
    if choice == "a":
        selected = wavs
    else:
        try:
            indices = [int(x) - 1 for x in choice.split()]
            selected = [wavs[i] for i in indices if 0 <= i < len(wavs)]
        except ValueError:
            print("Invalid input.")
            return

    overwrite = input("\nOverwrite originals? (y/N): ").strip().lower() == "y"
    print()

    for wav in selected:
        print(f"Processing: {wav.name}")
        process(wav, overwrite=overwrite)
        print()

    print("Done.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--file", type=str)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    if args.file:
        path = UPLOADS_DIR / args.file
        if not path.exists():
            print(f"File not found: {path}")
            return
        print(f"Processing: {path.name}")
        process(path, overwrite=args.overwrite)
    elif args.all:
        for wav in list_wavs():
            print(f"Processing: {wav.name}")
            process(wav, overwrite=args.overwrite)
            print()
        print("Done.")
    else:
        interactive()


if __name__ == "__main__":
    main()
