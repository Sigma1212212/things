#!/usr/bin/env python3
"""Procedural synthwave soundtrack for the Neon Tide trailer (pure Python).

    tools/trailer_music.py out.wav [seconds]

100 BPM, A minor (Am - F - C - G). Pads from the start, bass, arpeggio and
drums enter with the first night shot (7 s), a breakdown under the title card
(40 s) and a fade out. Deterministic (fixed random seed).
"""
import math, random, struct, sys, wave

RATE = 44100
out_path = sys.argv[1] if len(sys.argv) > 1 else "trailer_music.wav"
length = float(sys.argv[2]) if len(sys.argv) > 2 else 47.0
N = int(RATE * length)
L = [0.0] * N
R = [0.0] * N
rng = random.Random(7)

BPM = 100.0
BEAT = 60.0 / BPM
DRUMS_IN = 7.0
BREAK = 40.0

def midi(n): return 440.0 * 2 ** ((n - 69) / 12.0)

# chord roots (MIDI) and triads, one chord per bar (4 beats)
CHORDS = [(57, [57, 60, 64]), (53, [53, 57, 60]), (48, [48, 52, 55]), (55, [55, 59, 62])]

def add(buf, start, samples, gain):
    i0 = int(start * RATE)
    for k, v in enumerate(samples):
        i = i0 + k
        if 0 <= i < N: buf[i] += v * gain
        elif i >= N: break

def saw(f, t):
    x = (f * t) % 1.0
    return 2.0 * x - 1.0

def env_adsr(t, dur, a, d, s, r):
    if t < a: return t / a
    if t < a + d: return 1.0 - (1.0 - s) * (t - a) / d
    if t < dur: return s
    return max(0.0, s * (1.0 - (t - dur) / r))

def pad(start, dur, notes, gain):
    total = dur + 1.2
    n = int(total * RATE)
    outl, outr = [0.0] * n, [0.0] * n
    lp_l = lp_r = 0.0
    for k in range(n):
        t = k / RATE
        e = env_adsr(t, dur, 0.8, 0.5, 0.8, 1.2)
        vl = vr = 0.0
        for j, m in enumerate(notes):
            f = midi(m + 12)
            vl += saw(f * 1.004, t + j * 0.13)
            vr += saw(f * 0.996, t + j * 0.29)
        # gentle low-pass, slowly opening
        c = 0.035 + 0.02 * math.sin(t * 0.7)
        lp_l += c * (vl - lp_l)
        lp_r += c * (vr - lp_r)
        outl[k] = lp_l * e
        outr[k] = lp_r * e
    add(L, start, outl, gain)
    add(R, start, outr, gain)

def bass(start, dur, note, gain):
    n = int((dur + 0.05) * RATE)
    s = [0.0] * n
    f = midi(note - 12)
    lp = 0.0
    for k in range(n):
        t = k / RATE
        e = env_adsr(t, dur, 0.004, 0.12, 0.6, 0.05)
        v = saw(f, t) * 0.7 + math.sin(2 * math.pi * f * 0.5 * t) * 0.5
        c = 0.08 + 0.25 * math.exp(-t * 18)
        lp += c * (v - lp)
        s[k] = lp * e
    add(L, start, s, gain)
    add(R, start, s, gain)

def pluck(start, note, gain, pan):
    dur = 0.22
    n = int((dur + 0.25) * RATE)
    s = [0.0] * n
    f = midi(note + 12)
    for k in range(n):
        t = k / RATE
        e = math.exp(-t * 9)
        x = (f * t) % 1.0
        v = (1.0 if x < 0.3 else -1.0) * 0.6 + math.sin(2 * math.pi * f * 2 * t) * 0.3
        s[k] = v * e
    add(L, start, s, gain * (1 - pan))
    add(R, start, s, gain * pan)
    # echo (dotted eighth) for that 80s space
    add(L, start + BEAT * 0.75, s, gain * 0.35 * pan)
    add(R, start + BEAT * 0.75, s, gain * 0.35 * (1 - pan))

def kick(start, gain):
    n = int(0.4 * RATE)
    s = [0.0] * n
    ph = 0.0
    for k in range(n):
        t = k / RATE
        f = 45 + 110 * math.exp(-t * 30)
        ph += 2 * math.pi * f / RATE
        s[k] = math.sin(ph) * math.exp(-t * 7)
    add(L, start, s, gain)
    add(R, start, s, gain)

def snare(start, gain):
    n = int(0.35 * RATE)
    sl, sr = [0.0] * n, [0.0] * n
    for k in range(n):
        t = k / RATE
        tone = math.sin(2 * math.pi * 190 * t) * math.exp(-t * 25)
        noise = (rng.random() * 2 - 1) * math.exp(-t * 11)
        sl[k] = tone * 0.5 + noise * 0.7
        sr[k] = tone * 0.5 + (rng.random() * 2 - 1) * math.exp(-t * 11) * 0.7
    add(L, start, sl, gain)
    add(R, start, sr, gain)
    add(L, start + 0.09, sl, gain * 0.25)       # gated-reverb-ish tail
    add(R, start + 0.13, sr, gain * 0.25)

def hat(start, gain, open_=False):
    n = int((0.25 if open_ else 0.05) * RATE)
    s = [0.0] * n
    prev = 0.0
    for k in range(n):
        t = k / RATE
        x = rng.random() * 2 - 1
        hp = x - prev
        prev = x
        s[k] = hp * math.exp(-t * (12 if open_ else 70))
    add(L, start, s, gain * 0.8)
    add(R, start, s, gain)

bar_len = BEAT * 4
bars = int(length / bar_len) + 1
for b in range(bars):
    t0 = b * bar_len
    root, triad = CHORDS[b % 4]
    if t0 >= length: break
    pad(t0, bar_len, triad, 0.05)
    in_groove = DRUMS_IN - 0.01 <= t0 < BREAK
    if in_groove:
        for e in range(8):                        # eighth-note bass
            bass(t0 + e * BEAT / 2, BEAT / 2 * 0.8, root + (12 if e % 4 == 3 else 0), 0.22)
        arp = [triad[0], triad[1], triad[2], triad[1] + 12, triad[2], triad[1]]
        for s16 in range(16):                     # sixteenth arpeggio
            pluck(t0 + s16 * BEAT / 4, arp[s16 % len(arp)] + 12, 0.07, 0.3 + 0.4 * ((s16 * 5) % 7) / 6)
        for beat in range(4):
            kick(t0 + beat * BEAT, 0.55)
            if beat % 2 == 1: snare(t0 + beat * BEAT, 0.3)
            hat(t0 + beat * BEAT + BEAT / 2, 0.12, open_=(beat == 3))
            hat(t0 + beat * BEAT, 0.07)
    elif t0 < DRUMS_IN:
        # intro: heartbeat kick on the downbeat of the second bar
        if b == 1: kick(t0 + BEAT * 3, 0.35)
# riser into the groove and a final hit on the title card
for k in range(int(1.5 * RATE)):
    t = k / RATE
    i = int((DRUMS_IN - 1.5) * RATE) + k
    if 0 <= i < N:
        v = (rng.random() * 2 - 1) * (t / 1.5) ** 2 * 0.12
        L[i] += v
        R[i] += v
kick(BREAK, 0.7)
snare(BREAK, 0.4)
bass(BREAK, 2.5, 45, 0.3)

# master: fade in/out, soft clip, 16-bit
fade_in, fade_out = 0.8, 3.0
with wave.open(out_path, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    frames = bytearray()
    for i in range(N):
        t = i / RATE
        g = min(1.0, t / fade_in) * min(1.0, max(0.0, (length - t) / fade_out))
        l = math.tanh(L[i] * 1.3 * g)
        r = math.tanh(R[i] * 1.3 * g)
        frames += struct.pack("<hh", int(l * 30000), int(r * 30000))
    w.writeframes(bytes(frames))
print("wrote", out_path, "(%.1f s)" % length)
