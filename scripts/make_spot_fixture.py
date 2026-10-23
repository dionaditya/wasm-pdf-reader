#!/usr/bin/env python3
"""Separation / DeviceN fixture: a spot tint through a type-2 transform and a
two-ink DeviceN through a sampled (type 0) transform, both into DeviceCMYK."""
import importlib.util
import os

spec = importlib.util.spec_from_file_location(
    "mk", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_pattern_fixtures.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")

# Page 1: Separation spot "PANTONE 185" -> CMYK, tint 40% and 100%.
page1 = (b"/CS1 cs 0.4 scn 30 30 240 110 re f\n"
         b"/CS1 cs 1 scn 30 160 240 110 re f\n")
# Page 2: DeviceN with two inks, sampled transform (2 inputs -> 4 CMYK outputs).
page2 = (b"/CS2 cs 0.2 0.8 scn 30 30 240 240 re f\n")

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
    b"/Resources << /ColorSpace << /CS1 7 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
    b"/Resources << /ColorSpace << /CS2 9 0 R >> >> /Contents 6 0 R >>",
    mk.stream(page1),
    mk.stream(page2),
    b"[ /Separation /PANTONE#20185 /DeviceCMYK 8 0 R ]",
    b"<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0.92 0.9 0] /N 1 >>",
    b"[ /DeviceN [/Cyan /Magenta] /DeviceCMYK 10 0 R ]",
    mk.stream(bytes([0, 0, 0, 0,  255, 0, 0, 0,  0, 255, 0, 0,  255, 255, 0, 0]),
              b"/FunctionType 0 /Domain [0 1 0 1] /Size [2 2] /BitsPerSample 8 "
              b"/Range [0 1 0 1 0 1 0 1]"),
]
mk.build(objs, os.path.join(OUT, "spot.pdf"), 2)
