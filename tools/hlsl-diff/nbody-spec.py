#!/usr/bin/env python3
# Generate the nBodyGravity differential spec: cbuffer + initial particles.
# Deterministic initial state: particles on a small sphere with zero velocity,
# so the gravity simulation output is reproducible on both sides.
import sys

N = 256          # particles
TILES = 2        # ceil(N/128)
words = []
# cbuffer cbCS: uint4 g_param (N, TILES, 0, 0) + float4 g_paramf (0.1, 1.0, 0, 0)
words += [str(N), str(TILES), "0", "0", "0.1", "1.0", "0.0", "0.0"]
# particles: pos = (cos(a)*r, sin(a)*r, 0, 1), vel = (0,0,0,0) — r=1.0
import math
for i in range(N):
    a = 2*math.pi*i/N
    words += [f"{math.cos(a):.9f}", f"{math.sin(a):.9f}", "0.0", "1.0",
              "0.0", "0.0", "0.0", "0.0"]

spec = f"""kernel CSMain
group 128
grid {N}
buf 0 {' '.join(words[:8])}
buf 1 {' '.join(words[8:])}
out 2 {N*8}
dump 2
"""
sys.stdout.write(spec)
