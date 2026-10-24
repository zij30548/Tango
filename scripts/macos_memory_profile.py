#!/usr/bin/env python3
"""Measure isolated, owned Tango processes on macOS.

Example:
  python3 scripts/macos_memory_profile.py \
      --binary build-macos/Tango.app/Contents/MacOS/Tango \
      --label before --output /tmp/musicplayer-memory-before

Each sample uses a fresh data directory and process. The playback case imports
and automatically plays a real 12-second, very quiet PCM WAV. This script never
signals a process it did not create. Raw output and metadata remain in --output.
For a large library, add --tracks 1000 --scenarios playback; distinct hard-link
paths share the same audio bytes without allocating hundreds of MB on disk.
"""

import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import sqlite3
import statistics
import subprocess
import sys
import time
import wave


def command(argv, output, timeout=10):
    started = time.monotonic()
    try:
        result = subprocess.run(argv, text=True, capture_output=True, timeout=timeout)
        contents = result.stdout + result.stderr
        status = result.returncode
    except subprocess.TimeoutExpired as error:
        contents = str(error)
        status = "timeout"
    output.write_text(contents)
    return contents, status, round(time.monotonic() - started, 3)


def make_wave(path):
    rate = 22050
    frames = array.array("h", (int(50 * math.sin(2 * math.pi * 220 * n / rate))
                              for n in range(rate * 12)))
    if sys.byteorder != "little":
        frames.byteswap()
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(rate)
        audio.writeframes(frames.tobytes())


def history_count(path):
    if not path.exists():
        return None
    try:
        with sqlite3.connect(path) as db:
            return db.execute("SELECT COUNT(*) FROM play_history").fetchone()[0]
    except sqlite3.Error:
        return None


def summarize(samples, key):
    values = [sample[key] for sample in samples if sample.get(key) is not None]
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--settle-seconds", type=float, default=5.0)
    parser.add_argument("--tracks", type=int, default=1,
                        help="Number of unique WAV paths imported in the playback scenario")
    parser.add_argument("--scenarios", nargs="+", choices=("idle", "playback"),
                        default=["idle", "playback"])
    args = parser.parse_args()
    if args.tracks < 1 or args.repeats < 1:
        parser.error("--tracks and --repeats must be positive")
    binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    audio_file = args.output / "quiet-12s.wav"
    make_wave(audio_file)
    imported_files = [audio_file.resolve()]
    if args.tracks > 1:
        library = args.output / "library"
        library.mkdir()
        imported_files = []
        for index in range(args.tracks):
            track = library / f"track-{index + 1:04d}.wav"
            # 硬链接共享 PCM 数据，同时保留不同的规范路径。
            # 符号链接会被应用导入器去重，因此这里使用硬链接模拟多首曲目。
            os.link(audio_file, track)
            imported_files.append(track.resolve())
    report = {
        "label": args.label,
        "binary": str(binary),
        "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "timestamp_local": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "settle_seconds": args.settle_seconds,
        "tracks_in_playback": args.tracks,
        "wave": {"seconds": 12, "sample_rate": 22050, "channels": 1,
                 "bits": 16, "peak_amplitude": 50, "frequency_hz": 220},
        "samples": [],
    }
    for scenario in args.scenarios:
        for index in range(1, args.repeats + 1):
            sample_dir = args.output / f"{scenario}-{index}"
            sample_dir.mkdir()
            data_dir = sample_dir / "data"
            data_dir.mkdir()
            env = os.environ.copy()
            env["MUSICPLAYER_DATA_DIR"] = str(data_dir)
            argv = [str(binary)]
            if scenario == "playback":
                argv.extend(str(path) for path in imported_files)
            with (sample_dir / "app.log").open("w") as log:
                process = subprocess.Popen(argv, env=env, stdout=log, stderr=log)
                launched = time.monotonic()
                try:
                    time.sleep(args.settle_seconds)
                    if process.poll() is not None:
                        raise RuntimeError(f"App exited early: {sample_dir / 'app.log'}")
                    sample = {"scenario": scenario, "repeat": index, "pid": process.pid,
                              "sample_age_seconds": round(time.monotonic() - launched, 3)}
                    rss, status, elapsed = command(
                        ["ps", "-o", "rss=", "-p", str(process.pid)], sample_dir / "ps.txt")
                    sample.update(rss_kib=int(rss.strip()) if status == 0 else None,
                                  ps_status=status, ps_seconds=elapsed)
                    footprint, status, elapsed = command(
                        ["footprint", "-p", str(process.pid)], sample_dir / "footprint.txt")
                    sample.update(footprint_status=status, footprint_seconds=elapsed)
                    vmmap, status, elapsed = command(
                        ["vmmap", "-summary", str(process.pid)], sample_dir / "vmmap.txt")
                    sample.update(vmmap_status=status, vmmap_seconds=elapsed)
                    match = re.search(r"^Physical footprint:\s*([\d.]+)([KMGT]?)", vmmap, re.M)
                    if match:
                        units = {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
                        sample["physical_footprint_mib"] = float(match[1]) * units[match[2]] / 1024**2
                    else:
                        sample["physical_footprint_mib"] = None
                    sample["history_rows"] = history_count(data_dir / "history.db")
                    sample["finished_age_seconds"] = round(time.monotonic() - launched, 3)
                    report["samples"].append(sample)
                    print(json.dumps(sample, ensure_ascii=False), flush=True)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)
            time.sleep(0.5)
    report["summary"] = {
        scenario: {key: summarize([s for s in report["samples"] if s["scenario"] == scenario], key)
                   for key in ("rss_kib", "physical_footprint_mib")}
        for scenario in args.scenarios
    }
    (args.output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
