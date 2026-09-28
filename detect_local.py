#!/usr/bin/env python3
"""Local drone DOA on the het68 detection cube, drawn in 3D.

The array is the same octahedral cube as firmware (`doa.c`): one ICS-43434 at
the centre of each face, cube standing on a vertex. This replaces a flat
six-sided (hexagon) radar with that cube and places the detected drone on the
direction-of-arrival ray.

A single node measures direction only. The marker distance is a display radius
(`--range`, default 1 m), not a measured range.

Firmware for a 128 mm cube:

    HET68_DOA_EDGE_MM=128 ./build.sh

Examples:

    ./detect_local.py --self-test
    ./detect_local.py --demo --snapshot /tmp/cube.png
    ./detect_local.py                      # live USB sound card
    ./detect_local.py --wav capture.wav
    ./detect_local.py --uart /dev/ttyACM0  # plot firmware SRC lines
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass

import numpy as np

FS = 48000
C_SOUND = 343.0
N = 256
FILT_SETTLE = 48
CONF_MIN = 0.28
DRONE_RMS = 2.5
WIND_RATIO = 0.38
CREST_MAX = 5.5
DEFAULT_EDGE_MM = 128
DEFAULT_RANGE_M = 1.0

# Face-centre unit directions. Mics 1..6. Opposite faces are negatives.
# Upper ring elevation +35.26°, lower ring −35.26°. Azimuth 0° = mic 1 / +X.
MIC_DIR = np.array(
    [
        [0.81650, 0.00000, 0.57735],
        [-0.40825, 0.70711, 0.57735],
        [-0.40825, -0.70711, 0.57735],
        [-0.81650, 0.00000, -0.57735],
        [0.40825, -0.70711, -0.57735],
        [0.40825, 0.70711, -0.57735],
    ],
    dtype=np.float64,
)

# Same coefficients as doa.c (Butterworth biquads, DF2T, 48 kHz).
HPF800 = (9.2862377786e-01, -1.8572475557e00, 9.2862377786e-01, -1.8521464854e00, 8.6234862603e-01)
LPF6000 = (9.7631072938e-02, 1.9526214588e-01, 9.7631072938e-02, -9.4280904158e-01, 3.3333333333e-01)
LPF250 = (2.6165269507e-04, 5.2330539013e-04, 2.6165269507e-04, -1.9537279491e00, 9.5477455992e-01)


@dataclass
class Fix:
    ok: bool
    az: float = 0.0
    el: float = 0.0
    conf: float = 0.0
    lvl_db: float = -120.0
    direction: np.ndarray | None = None


def max_lag(edge_mm: int) -> int:
    """Match DOA_MAXLAG in doa.c (cold-air 300 m/s floor, +2)."""
    return (edge_mm * FS + 300_000 - 1) // 300_000 + 2


def mic_positions(edge_mm: int) -> np.ndarray:
    """Face centres, metres. Radius is half the cube edge."""
    return MIC_DIR * (edge_mm * 0.001 * 0.5)


def cube_vertices(edge_mm: int) -> np.ndarray:
    """Eight corners. Face normals n1,n2,n3 are orthonormal, so
    vertices are (edge/2) * (±n1 ± n2 ± n3)."""
    s = edge_mm * 0.001 * 0.5
    n1, n2, n3 = MIC_DIR[0], MIC_DIR[1], MIC_DIR[2]
    verts = []
    for s1 in (-1.0, 1.0):
        for s2 in (-1.0, 1.0):
            for s3 in (-1.0, 1.0):
                verts.append(s * (s1 * n1 + s2 * n2 + s3 * n3))
    return np.stack(verts, axis=0)


def _edge_len(verts: np.ndarray) -> float:
    # Body diagonal is the longest; edge = diagonal / sqrt(3).
    dmax = 0.0
    for i in range(8):
        for j in range(i + 1, 8):
            dmax = max(dmax, float(np.linalg.norm(verts[i] - verts[j])))
    return dmax / math.sqrt(3.0)


def biquad(x: np.ndarray, coef: tuple[float, float, float, float, float]) -> np.ndarray:
    b0, b1, b2, a1, a2 = coef
    y = np.empty_like(x)
    z1 = 0.0
    z2 = 0.0
    for i, xv in enumerate(x):
        yv = b0 * xv + z1
        z1 = b1 * xv - a1 * yv + z2
        z2 = b2 * xv - a2 * yv
        y[i] = yv
    return y


def direction_from_azel(az_deg: float, el_deg: float) -> np.ndarray:
    az = math.radians(az_deg)
    el = math.radians(el_deg)
    ce = math.cos(el)
    return np.array([ce * math.cos(az), ce * math.sin(az), math.sin(el)], dtype=np.float64)


def azel_from_direction(d: np.ndarray) -> tuple[float, float]:
    az = math.degrees(math.atan2(d[1], d[0]))
    if az < 0.0:
        az += 360.0
    el = math.degrees(math.asin(max(-1.0, min(1.0, float(d[2])))))
    return az, el


def _parabolic(y1: float, y2: float, y3: float) -> float:
    denom = y1 - 2.0 * y2 + y3
    if abs(denom) < 1e-12:
        return 0.0
    d = 0.5 * (y1 - y3) / denom
    if -1.0 < d < 1.0:
        return d
    return 0.0


def xcorr_delay(a: np.ndarray, b: np.ndarray, eb: float, lo: int, hi: int, maxlag: int) -> tuple[float, float]:
    best = -1e30
    bestlag = 0
    corr = np.empty(2 * maxlag + 1, dtype=np.float64)
    aa = a[lo:hi]
    for lag in range(-maxlag, maxlag + 1):
        s = float(np.dot(aa, b[lo + lag : hi + lag]))
        corr[lag + maxlag] = s
        if s > best:
            best = s
            bestlag = lag
    lagf = float(bestlag)
    if -maxlag < bestlag < maxlag:
        lagf += _parabolic(corr[bestlag + maxlag - 1], corr[bestlag + maxlag], corr[bestlag + maxlag + 1])
    norm = math.sqrt(max(eb, 0.0) * float(np.dot(aa, aa))) + 1e-6
    conf = best / norm
    if conf < 0.0:
        conf = 0.0
    elif conf > 1.0:
        conf = 1.0
    return lagf, conf


def prepare_drone(window: np.ndarray, maxlag: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """window: (6, N) float. Returns filtered window, per-channel energy, active mask."""
    lo = maxlag + FILT_SETTLE
    hi = N - maxlag
    work = np.empty_like(window)
    energy = np.zeros(6, dtype=np.float64)
    active = np.zeros(6, dtype=bool)
    crest_num = 0.0
    crest_den = 0.0
    for c in range(6):
        x = window[c] - float(window[c].mean())
        wind = biquad(x, LPF250)
        y = biquad(biquad(x, HPF800), LPF6000)
        work[c] = y
        sl = slice(FILT_SETTLE, None)
        e_bp = float(np.dot(y[sl], y[sl]))
        e_wind = float(np.dot(wind[sl], wind[sl]))
        peak = float(np.max(np.abs(y[sl])))
        energy[c] = float(np.dot(y[lo:hi], y[lo:hi]))
        rms = math.sqrt(e_bp / (N - FILT_SETTLE))
        active[c] = rms > DRONE_RMS and e_bp >= WIND_RATIO * (e_wind + 1e-6)
        crest_num += peak
        crest_den += rms + 1e-6
    if crest_num / crest_den > CREST_MAX:
        active[:] = False
    return work, energy, active


def solve_tdoa(work: np.ndarray, energy: np.ndarray, active: np.ndarray, pos: np.ndarray, maxlag: int, c_sound: float) -> Fix:
    idx = np.flatnonzero(active)
    if idx.size < 4:
        return Fix(False)
    ref = int(idx[np.argmax(energy[idx])])
    lo = maxlag + FILT_SETTLE
    hi = N - maxlag
    ata = np.zeros((3, 3), dtype=np.float64)
    atb = np.zeros(3, dtype=np.float64)
    conf_sum = 0.0
    conf_n = 0
    eref = float(energy[ref])
    for i in idx:
        i = int(i)
        if i == ref:
            continue
        lag, conf = xcorr_delay(work[ref], work[i], float(energy[i]), lo, hi, maxlag)
        row = pos[i] - pos[ref]
        # Same model as doa.c: (p_i - p_ref) · d = -(c/fs) * lag
        bb = -(c_sound / FS) * lag
        w = conf
        ata += w * np.outer(row, row)
        atb += w * row * bb
        conf_sum += conf
        conf_n += 1
    try:
        d = np.linalg.solve(ata, atb)
    except np.linalg.LinAlgError:
        return Fix(False)
    mag = float(np.linalg.norm(d))
    if mag < 1e-6:
        return Fix(False)
    d = d / mag
    az, el = azel_from_direction(d)
    conf = conf_sum / conf_n if conf_n else 0.0
    rms = math.sqrt(eref / (hi - lo))
    lvl = 20.0 * math.log10((rms + 1e-6) / 32768.0)
    return Fix(conf > 0.12, az, el, conf, lvl, d)


def detect_window(samples: np.ndarray, edge_mm: int, c_sound: float = C_SOUND) -> Fix:
    """samples: (6, N) int-like. Drone-band TDOA on the detection cube."""
    ml = max_lag(edge_mm)
    if ml + FILT_SETTLE >= N - ml:
        raise ValueError(f"edge {edge_mm} mm does not fit the {N}-sample window")
    work, energy, active = prepare_drone(samples.astype(np.float64), ml)
    return solve_tdoa(work, energy, active, mic_positions(edge_mm), ml, c_sound)


def synth_window(az: float, el: float, edge_mm: int, n: int = N, amp: float = 4000.0, seed: int = 1) -> np.ndarray:
    """Broadband noise with fractional delays of a plane wave from az/el."""
    rng = np.random.default_rng(seed)
    src = rng.normal(0.0, 1.0, n + 64)
    # Colour it a bit so the drone band has energy.
    src = np.convolve(src, [0.2, 0.6, 0.2], mode="same")
    d = direction_from_azel(az, el)
    pos = mic_positions(edge_mm)
    out = np.zeros((6, n), dtype=np.float64)
    for i in range(6):
        # τ = -(p · d) / c is negative when the mic faces the source (hears early).
        # y[n] = x[n - τ] so an early mic reads further ahead in the source buffer.
        tau = -float(np.dot(pos[i], d)) / C_SOUND * FS
        base = 32.0 - tau
        i0 = int(math.floor(base))
        frac = base - i0
        for t in range(n):
            j = i0 + t
            out[i, t] = amp * ((1.0 - frac) * src[j] + frac * src[j + 1])
    return out


def ang_err(a: float, b: float) -> float:
    d = abs(a - b) % 360.0
    return min(d, 360.0 - d)


def self_test(edge_mm: int) -> None:
    verts = cube_vertices(edge_mm)
    edge = _edge_len(verts)
    if abs(edge - edge_mm * 0.001) > 1e-4:
        raise SystemExit(f"cube edge {edge:.4f} m != {edge_mm} mm")
    # Three orthonormal face normals.
    if abs(float(np.dot(MIC_DIR[0], MIC_DIR[1]))) > 1e-3:
        raise SystemExit("mic 1 and 2 are not orthogonal")
    cases = [(0.0, 0.0), (90.0, 10.0), (210.0, 25.0), (330.0, -15.0)]
    for az, el in cases:
        fix = detect_window(synth_window(az, el, edge_mm), edge_mm)
        if not fix.ok or fix.conf < CONF_MIN:
            raise SystemExit(f"no lock for az={az} el={el} conf={fix.conf:.2f}")
        if ang_err(fix.az, az) > 8.0 or abs(fix.el - el) > 8.0:
            raise SystemExit(f"miss az={az}->{fix.az:.1f} el={el}->{fix.el:.1f}")
        print(f"OK  az={az:6.1f}->{fix.az:6.1f}  el={el:6.1f}->{fix.el:6.1f}  conf={fix.conf:.2f}")
    print(f"self-test OK  edge={edge_mm} mm  vertices={len(verts)}  edges checked")


class CubeView:
    """3D detection cube plus a drone marker on the DOA ray."""

    def __init__(self, edge_mm: int, range_m: float):
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d.art3d import Line3D

        self.plt = plt
        self.edge_mm = edge_mm
        self.range_m = range_m
        self.verts = cube_vertices(edge_mm)
        self.edges = []
        # Rebuild edges by exact length, not the helper's first-pass bug risk.
        target = edge_mm * 0.001
        for i in range(8):
            for j in range(i + 1, 8):
                if abs(float(np.linalg.norm(self.verts[i] - self.verts[j])) - target) < 1e-4:
                    self.edges.append((i, j))
        if len(self.edges) != 12:
            raise RuntimeError(f"expected 12 cube edges, got {len(self.edges)}")

        self.fig = plt.figure(figsize=(8.2, 7.2), facecolor="#0e1114")
        self.ax = self.fig.add_subplot(111, projection="3d", facecolor="#0e1114")
        self.ax.set_facecolor("#0e1114")
        self.ax.tick_params(colors="#9aa7b4")
        self.ax.xaxis.label.set_color("#9aa7b4")
        self.ax.yaxis.label.set_color("#9aa7b4")
        self.ax.zaxis.label.set_color("#9aa7b4")
        for axis in (self.ax.xaxis, self.ax.yaxis, self.ax.zaxis):
            axis.pane.set_facecolor((0.08, 0.09, 0.11, 1.0))
            axis.pane.set_edgecolor((0.18, 0.22, 0.27, 1.0))
        span = max(range_m * 1.15, 0.4)
        self.ax.set_xlim(-span, span)
        self.ax.set_ylim(-span, span)
        self.ax.set_zlim(-span, span)
        self.ax.set_xlabel("X  sever / az 0°  [m]")
        self.ax.set_ylabel("Y  [m]")
        self.ax.set_zlabel("Z  nahoru  [m]")
        self.ax.set_box_aspect((1, 1, 1))
        self.title = self.ax.set_title(
            f"het68  detekční krychle {edge_mm} mm",
            color="#f4f7fa",
            fontsize=13,
            pad=12,
        )
        for i, j in self.edges:
            seg = np.stack([self.verts[i], self.verts[j]])
            self.ax.plot(seg[:, 0], seg[:, 1], seg[:, 2], color="#3cb4f0", linewidth=1.6)
        pos = mic_positions(edge_mm)
        self.ax.scatter(pos[:, 0], pos[:, 1], pos[:, 2], c="#f4f7fa", s=18, depthshade=False)
        for k in range(6):
            self.ax.text(pos[k, 0], pos[k, 1], pos[k, 2], f"  {k+1}", color="#9aa7b4", fontsize=8)
        origin = np.zeros(3)
        self.ray = Line3D([0, 0], [0, 0], [0, 0], color="#e7b15a", linewidth=1.2)
        self.ax.add_line(self.ray)
        self.drone = self.ax.scatter([0], [0], [0], c="#e07a6a", s=40, depthshade=False)
        self.drone.set_visible(False)
        # Four rotor arms in the horizontal plane so the marker reads as a drone.
        self.arms = [
            self.ax.plot([0, 0], [0, 0], [0, 0], color="#e07a6a", linewidth=2.0)[0]
            for _ in range(4)
        ]
        for arm in self.arms:
            arm.set_visible(False)
        self.ray.set_visible(False)
        self._origin = origin

    def update(self, fix: Fix | None) -> None:
        if fix is None or not fix.ok or fix.direction is None or fix.conf < CONF_MIN:
            self.drone.set_visible(False)
            self.ray.set_visible(False)
            for arm in self.arms:
                arm.set_visible(False)
            self.title.set_text(f"het68  detekční krychle {self.edge_mm} mm   ·   žádný dron")
            return
        p = fix.direction * self.range_m
        self.ray.set_data_3d([0.0, p[0]], [0.0, p[1]], [0.0, p[2]])
        self.ray.set_visible(True)
        self.drone._offsets3d = ([p[0]], [p[1]], [p[2]])
        self.drone.set_visible(True)
        arm = self.range_m * 0.06
        offsets = ((arm, arm, 0.0), (arm, -arm, 0.0), (-arm, arm, 0.0), (-arm, -arm, 0.0))
        for line, (dx, dy, dz) in zip(self.arms, offsets):
            line.set_data_3d([p[0], p[0] + dx], [p[1], p[1] + dy], [p[2], p[2] + dz])
            line.set_visible(True)
        self.title.set_text(
            f"dron   az {fix.az:5.1f}°   el {fix.el:5.1f}°   conf {fix.conf:.2f}   {fix.lvl_db:.0f} dB"
            f"\npoloha na paprsku {self.range_m:.1f} m (jeden uzel neměří vzdálenost)"
        )

    def save(self, path: str) -> None:
        self.fig.savefig(path, dpi=120, facecolor=self.fig.get_facecolor())
        print(f"snapshot {path}")


def find_card() -> str | None:
    if not shutil.which("arecord"):
        return None
    try:
        out = subprocess.check_output(["arecord", "-l"], text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    m = re.search(r"card (\d+):.*Pico 6ch", out)
    if m:
        return m.group(1)
    return None


def read_s24_3le(buf: bytes) -> np.ndarray:
    """buf length must be a multiple of 18 (6 ch * 3 bytes). Returns (6, frames) int32."""
    n = len(buf) // 18
    out = np.empty((6, n), dtype=np.int32)
    mv = memoryview(buf)
    for i in range(n):
        base = i * 18
        for c in range(6):
            b0, b1, b2 = mv[base + 3 * c : base + 3 * c + 3]
            v = b0 | (b1 << 8) | (b2 << 16)
            if v & 0x800000:
                v -= 0x1000000
            out[c, i] = v >> 8  # match firmware ring (s24 >> 8 into int16 range)
    return out


def iter_arecord(device: str):
    cmd = [
        "arecord",
        "-D",
        device,
        "-f",
        "S24_3LE",
        "-r",
        str(FS),
        "-c",
        "6",
        "-t",
        "raw",
        "--period-size=480",
        "--buffer-size=4800",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    assert proc.stdout is not None
    frame = 18
    need = N * frame
    buf = bytearray()
    try:
        while True:
            chunk = proc.stdout.read(need)
            if not chunk:
                break
            buf.extend(chunk)
            while len(buf) >= need:
                yield read_s24_3le(bytes(buf[:need]))
                del buf[:need]
    finally:
        proc.kill()
        proc.wait(timeout=2)


def iter_wav(path: str):
    import wave

    with wave.open(path, "rb") as w:
        if w.getnchannels() != 6 or w.getframerate() != FS:
            raise SystemExit(f"{path}: need 6 ch @ {FS} Hz, got {w.getnchannels()} ch @ {w.getframerate()}")
        sw = w.getsampwidth()
        while True:
            raw = w.readframes(N)
            if len(raw) < N * 6 * sw:
                break
            if sw == 2:
                ints = np.frombuffer(raw, dtype="<i2").reshape(-1, 6).T
            elif sw == 3:
                ints = read_s24_3le(raw)
            else:
                raise SystemExit(f"unsupported sample width {sw}")
            yield ints.astype(np.int32)


SRC_RE = re.compile(
    r"SRC class=drone(?: id=(\d+))? az=([+-]?\d+(?:\.\d+)?) el=([+-]?\d+(?:\.\d+)?)"
    r"(?: conf=([+-]?\d+(?:\.\d+)?))?(?: lvl=([+-]?\d+(?:\.\d+)?))?"
)


def iter_uart(port: str):
    import serial  # pyserial, optional

    ser = serial.Serial(port, 115200, timeout=0.5)
    buf = ""
    try:
        while True:
            buf += ser.read(256).decode("utf-8", errors="ignore")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                m = SRC_RE.search(line)
                if not m:
                    continue
                az = float(m.group(2))
                el = float(m.group(3))
                conf = float(m.group(4)) if m.group(4) else 1.0
                lvl = float(m.group(5)) if m.group(5) else -30.0
                yield Fix(True, az, el, conf, lvl, direction_from_azel(az, el))
    finally:
        ser.close()


def demo_frames(edge_mm: int):
    t0 = time.time()
    while True:
        t = time.time() - t0
        az = (t * 40.0) % 360.0
        el = 15.0 * math.sin(t * 0.7)
        samples = synth_window(az, el, edge_mm, seed=int(t * 5) % 10000 + 1)
        fix = detect_window(samples, edge_mm)
        yield fix
        time.sleep(0.15)


def run_view(frames, edge_mm: int, range_m: float, snapshot: str | None, once: bool) -> None:
    os.environ.setdefault("MPLBACKEND", "Agg" if snapshot and not os.environ.get("DISPLAY") else os.environ.get("MPLBACKEND", ""))
    if snapshot and not os.environ.get("DISPLAY"):
        import matplotlib
        matplotlib.use("Agg")
    view = CubeView(edge_mm, range_m)
    if once or snapshot:
        fix = next(iter(frames))
        view.update(fix)
        if snapshot:
            view.save(snapshot)
        else:
            view.plt.show()
        return
    from matplotlib.animation import FuncAnimation

    state = {"fix": None}

    def pump(_frame):
        try:
            state["fix"] = next(frames)
        except StopIteration:
            return
        view.update(state["fix"])
        if state["fix"] and state["fix"].ok:
            print(
                f"SRC class=drone az={state['fix'].az:.1f} el={state['fix'].el:.1f} "
                f"conf={state['fix'].conf:.2f} lvl={state['fix'].lvl_db:.1f}dB"
            )

    FuncAnimation(view.fig, pump, interval=200, cache_frame_data=False)
    view.plt.show()


def main() -> None:
    p = argparse.ArgumentParser(description="Local 3D drone detection on the het68 cube")
    p.add_argument("--edge-mm", type=int, default=DEFAULT_EDGE_MM, help="cube edge in mm (default 128)")
    p.add_argument("--range", type=float, default=DEFAULT_RANGE_M, dest="range_m", help="display radius in metres")
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--demo", action="store_true", help="synthetic drone, no sound card")
    p.add_argument("--wav", help="6-channel 48 kHz WAV")
    p.add_argument("--device", help="ALSA device, default hw:<Pico 6ch>,0")
    p.add_argument("--uart", help="plot firmware SRC lines from a serial port")
    p.add_argument("--snapshot", help="write one PNG and exit")
    p.add_argument("--once", action="store_true", help="one window, then show or exit")
    args = p.parse_args()

    if args.self_test:
        self_test(args.edge_mm)
        return

    if args.uart:
        frames = iter_uart(args.uart)
    elif args.demo or args.snapshot and not args.wav and not args.device:
        frames = demo_frames(args.edge_mm)
    elif args.wav:
        frames = (detect_window(w, args.edge_mm) for w in iter_wav(args.wav))
    else:
        card = find_card()
        dev = args.device or (f"hw:{card},0" if card else None)
        if not dev:
            raise SystemExit("není Pico 6ch zvukovka. Použij --demo, --wav nebo --device.")
        print(f"capture {dev}")
        frames = (detect_window(w, args.edge_mm) for w in iter_arecord(dev))

    if args.snapshot and not os.environ.get("DISPLAY"):
        # One frame is enough for a headless snapshot.
        view_frames = frames
        run_view(view_frames, args.edge_mm, args.range_m, args.snapshot, once=True)
        return
    run_view(frames, args.edge_mm, args.range_m, args.snapshot, args.once)


if __name__ == "__main__":
    main()
