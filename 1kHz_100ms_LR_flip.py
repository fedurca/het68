#!/usr/bin/env python3
"""Play 1 kHz on every stereo playback output — alternate L/R every 100 ms.

Uses ALSA (aplay) and, when available, PipeWire/Pulse sinks (pw-cat or paplay).
No third-party Python packages required.

Examples:
  ./1kHz_100ms_LR_flip.py --list
  ./1kHz_100ms_LR_flip.py --duration 10
  ./1kHz_100ms_LR_flip.py --device hw:0,0
"""

from __future__ import annotations

import argparse
import math
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Iterable, Iterator, List, Optional

RATE = 48000
FREQ_HZ = 1000.0
BLOCK_MS = 100
AMPLITUDE = 0.45  # fraction of int16 full scale
CHANNELS = 2


@dataclass(frozen=True)
class PlaybackTarget:
    label: str
    argv: List[str]  # command that reads S16_LE stereo raw PCM from stdin


def _run_checked(cmd: List[str]) -> str:
    try:
        return subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"{' '.join(cmd)} failed:\n{exc.output}") from exc


def _alsa_playback_targets() -> List[PlaybackTarget]:
    if not shutil.which("aplay"):
        return []
    text = _run_checked(["aplay", "-l"])
    targets: List[PlaybackTarget] = []
    for line in text.splitlines():
        m = re.match(r"^card (\d+): .*, device (\d+):", line)
        if not m:
            continue
        card, dev = m.group(1), m.group(2)
        name = line.strip()
        targets.append(
            PlaybackTarget(
                label=f"ALSA {name}",
                argv=[
                    "aplay",
                    "-q",
                    "-D",
                    f"plughw:{card},{dev}",
                    "-f",
                    "S16_LE",
                    "-r",
                    str(RATE),
                    "-c",
                    str(CHANNELS),
                    "-t",
                    "raw",
                    "-",
                ],
            )
        )
    return targets


def _pipewire_targets() -> List[PlaybackTarget]:
    targets: List[PlaybackTarget] = []
    if shutil.which("pw-cli"):
        try:
            out = subprocess.check_output(
                ["pw-cli", "ls", "Node"],
                stderr=subprocess.STDOUT,
                text=True,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            out = ""
        # Node blocks with "node.name = " and "media.class = "Audio/Sink""
        blocks = out.split("\n\tid ")
        for block in blocks:
            if "Audio/Sink" not in block:
                continue
            nm = re.search(r'node\.name = "([^"]+)"', block)
            desc = re.search(r'node\.description = "([^"]+)"', block)
            if not nm:
                continue
            node = nm.group(1)
            label = desc.group(1) if desc else node
            if shutil.which("pw-cat"):
                targets.append(
                    PlaybackTarget(
                        label=f"PipeWire {label}",
                        argv=[
                            "pw-cat",
                            "--playback",
                            "--target",
                            node,
                            "--format",
                            "s16",
                            "--rate",
                            str(RATE),
                            "--channels",
                            str(CHANNELS),
                            "-",
                        ],
                    )
                )
    if shutil.which("pactl"):
        try:
            out = subprocess.check_output(
                ["pactl", "list", "short", "sinks"],
                stderr=subprocess.STDOUT,
                text=True,
            )
        except subprocess.CalledProcessError:
            out = ""
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            sink_name = parts[1]
            if sink_name.endswith(".monitor"):
                continue
            if shutil.which("paplay"):
                targets.append(
                    PlaybackTarget(
                        label=f"Pulse {sink_name}",
                        argv=[
                            "paplay",
                            "--device",
                            sink_name,
                            "--format",
                            "s16le",
                            "--rate",
                            str(RATE),
                            "--channels",
                            str(CHANNELS),
                            "--raw",
                            "/dev/stdin",
                        ],
                    )
                )
    return targets


def list_targets() -> List[PlaybackTarget]:
    seen = set()
    out: List[PlaybackTarget] = []
    for t in _alsa_playback_targets() + _pipewire_targets():
        key = tuple(t.argv)
        if key in seen:
            continue
        seen.add(key)
        out.append(t)
    return out


def sine_block(left: bool) -> bytes:
    n = RATE * BLOCK_MS // 1000
    amp = int(AMPLITUDE * 32767.0)
    buf = bytearray(n * CHANNELS * 2)
    for i in range(n):
        s = int(round(amp * math.sin(2.0 * math.pi * FREQ_HZ * i / RATE)))
        lo = s & 0xFF
        hi = (s >> 8) & 0xFF
        off = i * 4
        if left:
            buf[off] = lo
            buf[off + 1] = hi
            buf[off + 2] = 0
            buf[off + 3] = 0
        else:
            buf[off] = 0
            buf[off + 1] = 0
            buf[off + 2] = lo
            buf[off + 3] = hi
    return bytes(buf)


def pcm_stream(stop: threading.Event) -> Iterator[bytes]:
    left = True
    while not stop.is_set():
        yield sine_block(left)
        left = not left


def play_target(target: PlaybackTarget, stop: threading.Event) -> None:
    proc = subprocess.Popen(
        target.argv,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    assert proc.stdin is not None
    try:
        for chunk in pcm_stream(stop):
            if stop.is_set():
                break
            try:
                proc.stdin.write(chunk)
                proc.stdin.flush()
            except BrokenPipeError:
                break
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.wait(timeout=2)
        if proc.returncode not in (0, -15, None) and proc.stderr:
            err = proc.stderr.read().decode(errors="replace").strip()
            if err:
                print(f"[{target.label}] {err}", file=sys.stderr)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="1 kHz tone on all stereo outputs, L/R flip every 100 ms."
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List playback targets and exit.",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        metavar="SEC",
        help="Stop after SEC seconds (default: run until Ctrl+C).",
    )
    parser.add_argument(
        "--device",
        action="append",
        metavar="SPEC",
        help="Play only this ALSA device (repeatable), e.g. hw:0,0 or plughw:3,0.",
    )
    args = parser.parse_args(argv)

    if args.device:
        targets = [
            PlaybackTarget(
                label=f"ALSA {d}",
                argv=[
                    "aplay",
                    "-q",
                    "-D",
                    d,
                    "-f",
                    "S16_LE",
                    "-r",
                    str(RATE),
                    "-c",
                    str(CHANNELS),
                    "-t",
                    "raw",
                    "-",
                ],
            )
            for d in args.device
        ]
    else:
        targets = list_targets()

    if args.list:
        if not targets:
            print("No stereo playback targets found (is PipeWire/ALSA up?).")
            return 1
        for i, t in enumerate(targets):
            print(f"{i}: {t.label}")
            print(f"   {' '.join(t.argv)}")
        return 0

    if not targets:
        print(
            "No playback devices found. Try: systemctl --user start pipewire wireplumber",
            file=sys.stderr,
        )
        return 1

    stop = threading.Event()

    def _handle_sig(_signum: int, _frame: object) -> None:
        stop.set()

    signal.signal(signal.SIGINT, _handle_sig)
    signal.signal(signal.SIGTERM, _handle_sig)

    print(
        f"1 kHz L/R flip every {BLOCK_MS} ms @ {RATE} Hz on {len(targets)} output(s). Ctrl+C to stop."
    )
    for t in targets:
        print(f"  -> {t.label}")

    threads = [
        threading.Thread(target=play_target, args=(t, stop), daemon=True)
        for t in targets
    ]
    for th in threads:
        th.start()

    if args.duration > 0:
        time.sleep(args.duration)
        stop.set()
    else:
        while not stop.is_set():
            time.sleep(0.1)

    for th in threads:
        th.join(timeout=3.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
