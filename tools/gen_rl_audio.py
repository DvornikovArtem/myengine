#!/usr/bin/env python3
"""Procedural sounds of the Rocket League game (docs/engine/audio.md).

Writes assets/audio/*.wav (PCM 16 bit, 44.1 kHz, mono; the music and the crowd are stereo) and assets/audio/manifest.json
(name, file, seconds, channels, loop, bytes). Everything is synthesised from sines, saw stacks and shaped noise: no sample
of anyone else is used. The loops are seamless by construction: tones have a whole number of cycles in the loop, noise
is made in the frequency domain (periodic by definition), and the music wraps its tails round the end of the loop.

Run from anywhere:  python tools/gen_rl_audio.py
Needs numpy and scipy.
"""

import json
import math
import wave
from pathlib import Path

import numpy as np
from scipy import signal

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "audio"
SR = 44100
RNG = np.random.default_rng(20251022)


# --------------------------------------------------------------------------- helpers
def t_axis(seconds):
    return np.arange(int(round(seconds * SR))) / SR


def sine(freq, seconds, phase=0.0):
    return np.sin(2 * np.pi * freq * t_axis(seconds) + phase)


def saw(freq, seconds, harmonics=24):
    """A band limited saw: a sum of sines (no aliasing)."""
    t = t_axis(seconds)
    out = np.zeros_like(t)
    for k in range(1, harmonics + 1):
        if freq * k > SR * 0.45:
            break
        out += np.sin(2 * np.pi * freq * k * t) / k
    return out * (2 / np.pi)


def noise(seconds):
    return RNG.standard_normal(int(round(seconds * SR)))


def periodic_noise(seconds, shape):
    """Noise that repeats exactly after `seconds`: random phases, the magnitude is shape(frequency in Hz)."""
    n = int(round(seconds * SR))
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    magnitude = np.array([shape(f) for f in freqs])
    phase = RNG.uniform(0, 2 * np.pi, len(freqs))
    spectrum = magnitude * np.exp(1j * phase)
    spectrum[0] = 0
    out = np.fft.irfft(spectrum, n)
    return out / (np.max(np.abs(out)) + 1e-9)


def lowpass(x, cutoff, order=4):
    return signal.sosfilt(signal.butter(order, cutoff / (SR / 2), "low", output="sos"), x)


def circular(fn, x):
    """A filter that wraps round the ends: three copies in a row, the middle one is the result (a loop stays a loop)."""
    n = len(x)
    return fn(np.tile(x, 3))[n:2 * n]


def highpass(x, cutoff, order=4):
    return signal.sosfilt(signal.butter(order, cutoff / (SR / 2), "high", output="sos"), x)


def bandpass(x, low, high, order=3):
    return signal.sosfilt(signal.butter(order, [low / (SR / 2), min(high, SR * 0.45) / (SR / 2)], "band", output="sos"), x)


def decay(seconds, tau, attack=0.002):
    t = t_axis(seconds)
    env = np.exp(-t / tau)
    if attack > 0:
        env *= np.minimum(1.0, t / attack)
    return env


def fade_out(x, seconds=0.01):
    n = min(len(x), int(seconds * SR))
    if n > 0:
        x = x.copy()
        x[-n:] *= np.linspace(1, 0, n)
    return x


def normalize(x, peak=0.85):
    top = np.max(np.abs(x)) + 1e-9
    return x * (peak / top)


def sweep(f0, f1, seconds, exponential=True):
    t = t_axis(seconds)
    if exponential:
        k = (f1 / f0) ** (1.0 / seconds)
        phase = 2 * np.pi * f0 * (k ** t - 1) / math.log(k)
    else:
        phase = 2 * np.pi * (f0 * t + 0.5 * (f1 - f0) * t * t / seconds)
    return np.sin(phase)


def pad_to(x, seconds):
    n = int(round(seconds * SR))
    return np.pad(x, (0, max(0, n - len(x))))[:n]


def mix(*layers):
    n = max(len(layer) for layer in layers)
    out = np.zeros(n)
    for layer in layers:
        out[:len(layer)] += layer
    return out


# --------------------------------------------------------------------------- the sounds
def make_engine_loop():
    """100 Hz base (441 samples per cycle): a harmonic stack, a sub, a 10 Hz throb and rumble; 2 s = 200 cycles."""
    seconds = 2.0
    t = t_axis(seconds)
    body = np.zeros_like(t)
    for k in range(1, 15):
        body += np.sin(2 * np.pi * 100 * k * t + 0.4 * k) / (k ** 1.25)
    body += 0.9 * np.sin(2 * np.pi * 50 * t)
    throb = 0.82 + 0.18 * np.sin(2 * np.pi * 10 * t)
    rumble = periodic_noise(seconds, lambda f: 0.0 if f < 20 else 1.0 / (1 + (f / 220.0) ** 2))
    x = body * throb + 0.35 * rumble * 3
    return normalize(circular(lambda y: lowpass(y, 6000, 2), x), 0.7)


def make_boost_loop():
    seconds = 1.5
    t = t_axis(seconds)
    hiss = periodic_noise(seconds, lambda f: 0.0 if f < 200 else (f / 1500.0) ** 0.3 / (1 + (f / 7000.0) ** 3))
    roar = periodic_noise(seconds, lambda f: 0.0 if f < 40 else 1.0 / (1 + (f / 500.0) ** 2))
    whistle = np.sin(2 * np.pi * 1200 * t + 3.0 * np.sin(2 * np.pi * 6 * t)) * 0.12
    x = 0.9 * hiss + 0.8 * roar + whistle
    return normalize(x, 0.7)


def make_ball_hit(pitch, tau, thump_gain):
    seconds = 0.45
    thump = sweep(190 * pitch, 55 * pitch, seconds) * decay(seconds, tau, 0.001)
    body = bandpass(noise(seconds), 500 * pitch, 1800 * pitch) * decay(seconds, 0.045, 0.0005)
    click = highpass(noise(0.02), 2500) * decay(0.02, 0.004, 0.0002)
    x = thump_gain * thump + 0.55 * body + 0.7 * pad_to(click, seconds)
    return fade_out(normalize(x, 0.9))


def make_ball_bounce():
    seconds = 0.3
    thump = sweep(140, 70, seconds) * decay(seconds, 0.07, 0.001)
    return fade_out(normalize(thump + 0.15 * bandpass(noise(seconds), 300, 900) * decay(seconds, 0.03), 0.6))


def make_jump():
    seconds = 0.35
    whoosh = bandpass(noise(seconds), 400, 3500) * np.sin(np.pi * np.minimum(1, t_axis(seconds) / seconds * 1.0)) ** 1.5
    chirp = sweep(280, 640, seconds) * decay(seconds, 0.12, 0.004)
    thump = sweep(120, 70, 0.1) * decay(0.1, 0.03)
    return fade_out(normalize(0.5 * whoosh + 0.6 * chirp + 0.5 * pad_to(thump, seconds), 0.7))


def make_dodge():
    seconds = 0.28
    t = t_axis(seconds)
    whoosh = bandpass(noise(seconds), 800, 6000) * np.sin(np.pi * np.minimum(1, t / seconds)) ** 1.2
    chirp = sweep(420, 1100, seconds) * decay(seconds, 0.09, 0.003)
    return fade_out(normalize(0.9 * whoosh + 0.45 * chirp, 0.7))


def make_land():
    seconds = 0.4
    thump = sweep(110, 45, seconds) * decay(seconds, 0.09, 0.001)
    dust = lowpass(noise(seconds), 900) * decay(seconds, 0.06, 0.001)
    return fade_out(normalize(thump + 0.5 * dust, 0.75))


def chime(freq, seconds, tau, harmonics=(1, 2.01, 3.02), weights=(1, 0.35, 0.12)):
    out = np.zeros(int(round(seconds * SR)))
    for h, w in zip(harmonics, weights):
        out += w * sine(freq * h, seconds)
    return out * decay(seconds, tau, 0.002)


def make_pad_small():
    notes = [(880, 0.00), (1320, 0.07)]
    x = np.zeros(int(0.5 * SR))
    for freq, start in notes:
        c = chime(freq, 0.35, 0.09)
        i = int(start * SR)
        x[i:i + len(c)] += c
    return fade_out(normalize(x, 0.7))


def make_pad_big():
    notes = [(440, 0.00), (660, 0.08), (880, 0.16), (1320, 0.24)]
    x = np.zeros(int(0.9 * SR))
    for freq, start in notes:
        c = chime(freq, 0.6, 0.16)
        i = int(start * SR)
        x[i:i + len(c)] += c
    x += 0.5 * pad_to(sweep(90, 220, 0.5) * decay(0.5, 0.2), 0.9)
    return fade_out(normalize(x, 0.8))


def make_demolish():
    seconds = 1.6
    t = t_axis(seconds)
    # the noise burst through a low pass that closes while the sound dies
    n = noise(seconds)
    out = np.zeros_like(n)
    block = 2048
    for i in range(0, len(n), block):
        cutoff = 7000 * math.exp(-3.2 * i / len(n)) + 150
        out[i:i + block] = lowpass(n[i:i + block], cutoff, 2)
    burst = out * decay(seconds, 0.35, 0.001)
    boom = sweep(95, 32, seconds) * decay(seconds, 0.45, 0.002)
    crackle = np.zeros_like(t)
    for _ in range(60):
        i = int(RNG.uniform(0.03, 1.0) * SR)
        crackle[i:i + 40] += RNG.standard_normal(40) * RNG.uniform(0.2, 1.0) * math.exp(-i / SR * 3)
    return fade_out(normalize(1.0 * burst + 1.2 * boom + 0.4 * highpass(crackle, 1500), 0.95), 0.05)


def make_goal_horn():
    seconds = 2.6
    t = t_axis(seconds)
    chord = np.zeros_like(t)
    for f in (220.0, 277.18, 329.63, 440.0):
        for detune in (0.997, 1.0, 1.004):
            chord += saw(f * detune, seconds, 14)
    vibrato = 1 + 0.015 * np.sin(2 * np.pi * 5.5 * t)
    env = np.minimum(1.0, t / 0.04) * np.where(t < 1.9, 1.0, np.exp(-(t - 1.9) / 0.22))
    x = lowpass(chord * vibrato, 3200, 2) * env
    return fade_out(normalize(x, 0.85), 0.05)


def make_crowd_roar():
    seconds = 4.0
    t = t_axis(seconds)
    left = bandpass(noise(seconds), 350, 3200)
    right = bandpass(noise(seconds), 350, 3200)
    swell = np.minimum(1.0, t / 0.5) * np.exp(-np.maximum(0, t - 1.1) / 1.2)
    shimmer = 0.85 + 0.15 * np.sin(2 * np.pi * 7 * t + 1)
    stereo = np.stack([left * swell * shimmer, right * swell * shimmer], axis=1)
    return fade_out_stereo(normalize_stereo(stereo, 0.85), 0.2)


def make_crowd_loop():
    seconds = 6.0
    shape = lambda f: 0.0 if f < 80 else (1.0 / (1 + (f / 900.0) ** 2)) * (1 + 0.6 * math.exp(-((f - 700) / 300.0) ** 2))
    left = periodic_noise(seconds, shape)
    right = periodic_noise(seconds, shape)
    t = t_axis(seconds)
    swell = 0.78 + 0.22 * np.sin(2 * np.pi * (1 / 3.0) * t + 0.5) + 0.08 * np.sin(2 * np.pi * (5 / 6.0) * t)
    return normalize_stereo(np.stack([left * swell, right * swell], axis=1), 0.6)


def normalize_stereo(x, peak):
    return x * (peak / (np.max(np.abs(x)) + 1e-9))


def fade_out_stereo(x, seconds):
    n = int(seconds * SR)
    x = x.copy()
    x[-n:] *= np.linspace(1, 0, n)[:, None]
    return x


def make_beep():
    seconds = 0.22
    return fade_out(normalize(sine(880, seconds) * decay(seconds, 0.4, 0.004) * np.minimum(1, (seconds - t_axis(seconds)) / 0.02 + 0.0), 0.7), 0.02)


def make_go():
    seconds = 0.7
    x = (sine(1320, seconds) + 0.6 * sine(1760, seconds) + 0.3 * sine(2640, seconds)) * decay(seconds, 0.25, 0.004)
    return fade_out(normalize(x, 0.75), 0.04)


def make_click(freq, seconds=0.06, tau=0.012):
    return fade_out(normalize((sine(freq, seconds) + 0.4 * sine(freq * 2.01, seconds)) * decay(seconds, tau, 0.0008), 0.6), 0.008)


def make_back():
    seconds = 0.09
    return fade_out(normalize(sweep(900, 450, seconds) * decay(seconds, 0.03, 0.001), 0.6), 0.01)


def make_menu_music():
    """120 BPM, 8 bars in A minor (Am F C G), 16 s, stereo; every tail wraps round the end so the loop is seamless."""
    bpm = 120
    beat = 60.0 / bpm
    bars = 8
    seconds = bars * 4 * beat
    n = int(round(seconds * SR))
    left = np.zeros(n)
    right = np.zeros(n)

    def put(buf, x, start_seconds):
        i = int(round(start_seconds * SR)) % n
        end = i + len(x)
        if end <= n:
            buf[i:end] += x
        else:
            buf[i:] += x[:n - i]
            buf[:end - n] += x[n - i:]

    def note_freq(midi):
        return 440.0 * 2 ** ((midi - 69) / 12.0)

    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]  # Am F C G
    for bar in range(bars):
        chord = chords[(bar // 2) % 4]
        start = bar * 4 * beat
        # the kick on every beat, the hat on the offbeats
        for b in range(4):
            kick = sweep(150, 45, 0.28) * decay(0.28, 0.07, 0.001)
            put(left, kick * 0.9, start + b * beat)
            put(right, kick * 0.9, start + b * beat)
            hat = highpass(noise(0.08), 6500) * decay(0.08, 0.018, 0.0005)
            put(left, hat * 0.18, start + b * beat + beat / 2)
            put(right, hat * 0.16, start + b * beat + beat / 2)
        # the bass: eighth notes on the root, an octave jump
        root = chord[0] - 24 if chord[0] > 50 else chord[0] - 12
        for e in range(8):
            midi = root + (12 if e % 4 == 3 else 0)
            tone = lowpass(saw(note_freq(midi), 0.28, 10), 700, 2) * decay(0.28, 0.12, 0.003)
            put(left, tone * 0.55, start + e * beat / 2)
            put(right, tone * 0.55, start + e * beat / 2)
        # the pad: the chord, slightly detuned and wide
        for midi in chord:
            for side, buf in ((0.995, left), (1.005, right)):
                tone = lowpass(saw(note_freq(midi + 12) * side, 4 * beat + 0.6, 12), 1800, 2)
                env = np.minimum(1.0, t_axis(4 * beat + 0.6) / 0.35) * np.exp(-np.maximum(0, t_axis(4 * beat + 0.6) - 4 * beat) / 0.2)
                put(buf, tone * env * 0.11, start)
        # the arpeggio plucks, sixteenth notes, a quiet echo on the other side
        order = [0, 1, 2, 1, 2, 1, 0, 2]
        for s in range(16):
            midi = chord[order[s % 8]] + 24
            pluck = (sine(note_freq(midi), 0.3) + 0.3 * sine(note_freq(midi) * 2, 0.3)) * decay(0.3, 0.07, 0.001)
            put(left, pluck * 0.16, start + s * beat / 4)
            put(right, pluck * 0.16, start + s * beat / 4 + 0.09)
    x = np.stack([left, right], axis=1)
    return normalize_stereo(x, 0.7)


# --------------------------------------------------------------------------- output
def write_wav(name, data):
    OUT.mkdir(parents=True, exist_ok=True)
    data = np.clip(data, -1.0, 1.0)
    channels = 1 if data.ndim == 1 else data.shape[1]
    pcm = (data * 32767).astype("<i2")
    path = OUT / (name + ".wav")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(channels)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes(pcm.tobytes())
    frames = len(pcm)
    return {"name": name, "file": name + ".wav", "seconds": round(frames / SR, 3), "channels": channels, "sampleRate": SR,
            "bytes": path.stat().st_size}


SOUNDS = [
    ("engine_loop", make_engine_loop, True),
    ("boost_loop", make_boost_loop, True),
    ("ball_hit_1", lambda: make_ball_hit(1.0, 0.11, 1.0), False),
    ("ball_hit_2", lambda: make_ball_hit(1.18, 0.09, 0.9), False),
    ("ball_hit_3", lambda: make_ball_hit(0.86, 0.14, 1.1), False),
    ("ball_bounce", make_ball_bounce, False),
    ("jump", make_jump, False),
    ("dodge", make_dodge, False),
    ("land", make_land, False),
    ("pad_small", make_pad_small, False),
    ("pad_big", make_pad_big, False),
    ("demolish", make_demolish, False),
    ("goal_horn", make_goal_horn, False),
    ("crowd_roar", make_crowd_roar, False),
    ("crowd_loop", make_crowd_loop, True),
    ("countdown_beep", make_beep, False),
    ("countdown_go", make_go, False),
    ("ui_click", lambda: make_click(1800), False),
    ("ui_hover", lambda: make_click(2600, 0.04, 0.008), False),
    ("ui_back", make_back, False),
    ("menu_music", make_menu_music, True),
]


def main():
    entries = []
    for name, build, loop in SOUNDS:
        entry = write_wav(name, build())
        entry["loop"] = loop
        entries.append(entry)
        print("  %-16s %6.2f s  %d ch  %7.1f KB%s" % (name, entry["seconds"], entry["channels"], entry["bytes"] / 1024.0, "  loop" if loop else ""))
    total = sum(e["bytes"] for e in entries)
    (OUT / "manifest.json").write_text(json.dumps({"sampleRate": SR, "sounds": entries, "totalBytes": total}, indent=1) + "\n", encoding="utf-8")
    print("total %.2f MB in %d files (limit 15 MB)" % (total / 1048576.0, len(entries)))
    return 0 if total <= 15 * 1048576 else 1


if __name__ == "__main__":
    raise SystemExit(main())
