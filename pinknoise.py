#!/usr/bin/env python3
"""Broadband pink noise on stereo playback outputs (lab / DOA tuning).

Default mode **both**: identical pink noise on L and R — one acoustic source,
best for GCC/TDOA direction finding with het68.

Optional modes for channel mapping (like 1kHz_100ms_LR_flip.py):
  --mode left|right|alternate

Uses ALSA (aplay) and PipeWire/Pulse when available. No pip packages.

Examples:
  ./pinknoise.py --list
  ./pinknoise.py --duration 30              # DOA: both channels, ~30 s
  ./pinknoise.py --amplitude 0.12           # quieter (~−18 dBFS peak target)
  ./pinknoise.py --mode alternate --duration 10   # L/R mapping test
  ./pinknoise.py --device plughw:0,0
"""

from __future__ import annotations

import argparse
import random
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from enum import Enum
from typing import Iterator, List, Optional

RATE = 48000
BLOCK_MS = 100
CHANNELS = 2
# Pink noise is broadband; keep lower than sine tone to avoid mic clipping.
DEFAULT_AMPLITUDE = 0.15


class OutputMode(str, Enum):
    BOTH = "both"          # mono pink -> L+R (DOA default)
    LEFT = "left"          # L only
    RIGHT = "right"        # R only
    ALTERNATE = "alternate"  # flip L/R every block (channel ID)


@dataclass(frozen=True)
class PlaybackTarget:
    label: str
    argv: List[str]


class PinkGenerator:
    """Paul Kellet pink-noise IIR (stdlib, stateful across blocks)."""

    __slots__ = ("_b0", "_b1", "_b2", "_b3", "_b4", "_b5", "_b6", "_rng")

    def __init__(self, seed: Optional[int] = None) -> None:
        self._b0 = self._b1 = self._b2 = self._b3 = 0.0
        self._b4 = self._b5 = self._b6 = 0.0
        self._rng = random.Random(seed)

    def _white(self) -> float:
        return self._rng.uniform(-1.0, 1.0)

    def sample(self) -> float:
        w = self._white()
        self._b0 = 0.99886 * self._b0 + w * 0.0555179
        self._b1 = 0.99332 * self._b1 + w * 0.0750759
        self._b2 = 0.96900 * self._b2 + w * 0.1538520
        self._b3 = 0.86650 * self._b3 + w * 0.3104856
        self._b4 = 0.55000 * self._b4 + w * 0.5329522
        self._b5 = -0.7616 * self._b5 - w * 0.0168980
        pink = (
            self._b0 + self._b1 + self._b2 + self._b3 + self._b4 + self._b5
            + self._b6 + w * 0.5362
        )
        self._b6 = w * 0.115926
        return pink


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
        targets.append(
            PlaybackTarget(
                label=f"ALSA {line.strip()}",
                argv=[
                    "aplay", "-q", "-D", f"plughw:{card},{dev}",
                    "-f", "S16_LE", "-r", str(RATE), "-c", str(CHANNELS),
                    "-t", "raw", "-",
                ],
            )
        )
    return targets


def _pipewire_targets() -> List[PlaybackTarget]:
    targets: List[PlaybackTarget] = []
    if shutil.which("pw-cli"):
        try:
            out = subprocess.check_output(
                ["pw-cli", "ls", "Node"], stderr=subprocess.STDOUT, text=True
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            out = ""
        for block in out.split("\n\tid "):
            if "Audio/Sink" not in block:
                continue
            nm = re.search(r'node\.name = "([^"]+)"', block)
            desc = re.search(r'node\.description = "([^"]+)"', block)
            if not nm or not shutil.which("pw-cat"):
                continue
            node = nm.group(1)
            label = desc.group(1) if desc else node
            targets.append(
                PlaybackTarget(
                    label=f"PipeWire {label}",
                    argv=[
                        "pw-cat", "--playback", "--target", node,
                        "--format", "s16", "--rate", str(RATE),
                        "--channels", str(CHANNELS), "-",
                    ],
                )
            )
    if shutil.which("pactl") and shutil.which("paplay"):
        try:
            out = subprocess.check_output(
                ["pactl", "list", "short", "sinks"],
                stderr=subprocess.STDOUT, text=True,
            )
        except subprocess.CalledProcessError:
            out = ""
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            sink = parts[1]
            if sink.endswith(".monitor"):
                continue
            targets.append(
                PlaybackTarget(
                    label=f"Pulse {sink}",
                    argv=[
                        "paplay", "--device", sink, "--format", "s16le",
                        "--rate", str(RATE), "--channels", str(CHANNELS),
                        "--raw", "/dev/stdin",
                    ],
                )
            )
    return targets


def list_targets() -> List[PlaybackTarget]:
    seen: set[tuple[str, ...]] = set()
    out: List[PlaybackTarget] = []
    for t in _alsa_playback_targets() + _pipewire_targets():
        key = tuple(t.argv)
        if key not in seen:
            seen.add(key)
            out.append(t)
    return out


def _clip16(v: float, amp: float) -> int:
    s = int(round(max(-1.0, min(1.0, v * amp)) * 32767.0))
    return max(-32768, min(32767, s))


def pink_block(
    gen: PinkGenerator,
    mode: OutputMode,
    amplitude: float,
    alternate_left: bool,
    block_ms: int,
) -> bytes:
    n = RATE * block_ms // 1000
    samples: List[int] = []
    for _ in range(n):
        p = gen.sample()
        l = _clip16(p, amplitude)
        r = l
        if mode == OutputMode.LEFT:
            r = 0
        elif mode == OutputMode.RIGHT:
            l = 0
        elif mode == OutputMode.ALTERNATE:
            if alternate_left:
                r = 0
            else:
                l = 0
        samples.extend((l, r))
    return struct.pack(f"<{len(samples)}h", *samples)


def pcm_stream(
    stop: threading.Event,
    mode: OutputMode,
    amplitude: float,
    seed: Optional[int],
    block_ms: int,
) -> Iterator[bytes]:
    gen = PinkGenerator(seed=seed)
    left = True
    while not stop.is_set():
        yield pink_block(gen, mode, amplitude, left, block_ms)
        if mode == OutputMode.ALTERNATE:
            left = not left


def play_target(
    target: PlaybackTarget,
    stop: threading.Event,
    mode: OutputMode,
    amplitude: float,
    seed: Optional[int],
    block_ms: int,
) -> None:
    proc = subprocess.Popen(
        target.argv,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    assert proc.stdin is not None
    try:
        for chunk in pcm_stream(stop, mode, amplitude, seed, block_ms):
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
        description="Pink noise on stereo outputs (default: same signal on L+R for DOA)."
    )
    parser.add_argument("--list", action="store_true", help="List outputs and exit.")
    parser.add_argument(
        "--duration", type=float, default=0.0, metavar="SEC",
        help="Stop after SEC seconds (default: until Ctrl+C).",
    )
    parser.add_argument(
        "--amplitude", type=float, default=DEFAULT_AMPLITUDE, metavar="A",
        help=f"Peak scale 0..1 (default {DEFAULT_AMPLITUDE}).",
    )
    parser.add_argument(
        "--mode", choices=[m.value for m in OutputMode], default=OutputMode.BOTH.value,
        help="both=same L+R (DOA), left/right=one channel, alternate=L/R flip per block.",
    )
    parser.add_argument(
        "--block-ms", type=int, default=BLOCK_MS, metavar="MS",
        help=f"Block size ms (default {BLOCK_MS}; used for alternate timing).",
    )
    parser.add_argument(
        "--seed", type=int, default=None, help="RNG seed for repeatable noise.",
    )
    parser.add_argument(
        "--device", action="append", metavar="SPEC",
        help="ALSA device only, e.g. plughw:0,0 (repeatable).",
    )
    args = parser.parse_args(argv)

    block_ms = max(10, args.block_ms)
    mode = OutputMode(args.mode)
    amp = max(0.0, min(1.0, args.amplitude))

    if args.device:
        targets = [
            PlaybackTarget(
                label=f"ALSA {d}",
                argv=[
                    "aplay", "-q", "-D", d, "-f", "S16_LE", "-r", str(RATE),
                    "-c", str(CHANNELS), "-t", "raw", "-",
                ],
            )
            for d in args.device
        ]
    else:
        targets = list_targets()

    if args.list:
        if not targets:
            print("No stereo playback targets found.")
            return 1
        for i, t in enumerate(targets):
            print(f"{i}: {t.label}")
            print(f"   {' '.join(t.argv)}")
        return 0

    if not targets:
        print("No playback devices found.", file=sys.stderr)
        return 1

    stop = threading.Event()

    def _sig(_s: int, _f: object) -> None:
        stop.set()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    mode_desc = {
        OutputMode.BOTH: "identical L+R (one source, for DOA)",
        OutputMode.LEFT: "left channel only",
        OutputMode.RIGHT: "right channel only",
        OutputMode.ALTERNATE: f"L/R alternate every {block_ms} ms",
    }[mode]

    print(
        f"Pink noise @ {RATE} Hz, amp={amp:.2f}, mode={mode.value}: {mode_desc}. "
        f"{len(targets)} output(s). Ctrl+C to stop."
    )
    for t in targets:
        print(f"  -> {t.label}")

    threads = [
        threading.Thread(
            target=play_target,
            args=(t, stop, mode, amp, args.seed, block_ms),
            daemon=True,
        )
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
