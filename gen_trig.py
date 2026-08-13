#!/usr/bin/env python3
"""Generate trig_table.h: a 256-step sin/cos lookup table, Q7 fixed-point
(values scaled by 128), used for the snake's continuous-angle movement."""
import math

N = 256
SCALE = 128

sin_vals = [max(-128, min(127, round(math.sin(2 * math.pi * i / N) * SCALE))) for i in range(N)]
cos_vals = [max(-128, min(127, round(math.cos(2 * math.pi * i / N) * SCALE))) for i in range(N)]

with open("trig_table.h", "w") as f:
    f.write("// Auto-generated sin/cos table, Q scaled by 128, 256 angle steps\n")
    f.write("static const s16 sinTable[256] = {\n    " + ", ".join(str(v) for v in sin_vals) + "\n};\n")
    f.write("static const s16 cosTable[256] = {\n    " + ", ".join(str(v) for v in cos_vals) + "\n};\n")

print("wrote trig_table.h")
