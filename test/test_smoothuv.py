#!/usr/bin/env python3
"""Functional tests for the SmoothUV VapourSynth plugin (API 4).

Usage: python test/test_smoothuv.py [path/to/smoothuv plugin]

Without an argument the plugin is expected to be autoloaded (e.g. from the installed wheel).
"""
import ctypes
import hashlib
import sys

import vapoursynth as vs

core = vs.core
FAILURES = []

# SHA-256 of both chroma planes of frame 0 of pattern_clip(100, 60) for (radius, threshold, interlaced);
# the SSE2 code (x86) and the scalar code (other CPUs) must give the same result.
REFERENCE = {
    (3, 270, False): "9daea6b87e343a6f6e4a508e4f6c2ec8db833788e68f2a4ac225f6e7bd83a4dc",
    (7, 450, True): "7d4f336fc4c31de1fb0dad8ce1633fa944a9659b421e359ee8d05f3fdf497f61",
    (1, 50, False): "f3599c5e19d6f24a855acacbbeed7d78d8cf47ae5d7f5cab1b7bb16738a9a850",
}


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL: " + msg)
        return
    print("ok  ", msg)


def expect_error(fn, msg, contains):
    try:
        fn()
    except vs.Error as e:
        check(contains in str(e), "%s (%s)" % (msg, str(e).strip().splitlines()[-1]))
    else:
        check(False, "no error raised: " + msg)


def pattern(w, h, n, p):
    """Gradient plus deterministic noise, with calmer patches."""
    rows = []
    state = (w * 7919 + h * 104729 + n * 1299709 + p * 15485863) & 0xFFFFFFFF
    for y in range(h):
        row = bytearray(w)
        for x in range(w):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (state >> 16) % 61 - 30
            base = 128 + ((x * 3 + y * 2 + n * 17 + p * 40) % 120) - 60
            if (x // 16 + y // 16) % 3 == 0:
                noise //= 10
            row[x] = max(0, min(255, base + noise))
        rows.append(bytes(row))
    return rows


def pattern_clip(w, h):
    blank = core.std.BlankClip(width=w, height=h, format=vs.YUV420P8, length=1)

    def fill(n, f):
        fout = f.copy()
        for p in range(3):
            pw, ph = (w, h) if p == 0 else (w // 2, h // 2)
            ptr = fout.get_write_ptr(p)
            ptr = ptr.value if hasattr(ptr, "value") else ptr
            stride = fout.get_stride(p)
            for y, row in enumerate(pattern(pw, ph, n, p)):
                ctypes.memmove(ptr + y * stride, row, pw)
        return fout

    return core.std.ModifyFrame(blank, blank, fill)


def plane(clip, p):
    return bytes(clip.get_frame(0)[p])


def chroma_digest(clip):
    f = clip.get_frame(0)
    return hashlib.sha256(bytes(f[1]) + bytes(f[2])).hexdigest()


def variance(data):
    mean = sum(data) / len(data)
    return sum((v - mean) ** 2 for v in data) / len(data)


def main():
    if len(sys.argv) > 1:
        core.std.LoadPlugin(sys.argv[1])
    check(hasattr(core, "smoothuv"), "plugin loaded (namespace smoothuv)")

    src = pattern_clip(100, 60)
    out = core.smoothuv.SmoothUV(src)
    check(out.format.id == src.format.id and out.num_frames == 1 and out.width == 100, "format, size and length kept")
    check(plane(out, 0) == plane(src, 0), "luma unchanged")
    check(variance(plane(out, 1)) < variance(plane(src, 1)) * 0.8, "chroma smoothed (variance %.0f -> %.0f)"
          % (variance(plane(src, 1)), variance(plane(out, 1))))

    for (radius, threshold, interlaced), expected in REFERENCE.items():
        digest = chroma_digest(core.smoothuv.SmoothUV(src, radius=radius, threshold=threshold, interlaced=interlaced))
        if expected is None:
            print("info reference radius=%d threshold=%d interlaced=%s: %s" % (radius, threshold, interlaced, digest))
        else:
            check(digest == expected, "reference result radius=%d threshold=%d interlaced=%s" % (radius, threshold, interlaced))

    # Flat chroma stays flat, also in the last columns when the chroma width is no multiple of 8
    # and in the last line of interlaced chroma with an odd height.
    for width, height, interlaced in ((852, 64, False), (706, 64, False), (864, 64, False), (12, 20, False), (720, 486, True)):
        flat = core.std.BlankClip(width=width, height=height, format=vs.YUV420P8, color=[128, 200, 60], length=1)
        res = core.smoothuv.SmoothUV(flat, interlaced=interlaced)
        u, v = plane(res, 1), plane(res, 2)
        check(set(u) == {200} and set(v) == {60}, "flat chroma unchanged at %dx%d interlaced=%s" % (width, height, interlaced))

    for threshold in (0, 1):
        res = core.smoothuv.SmoothUV(src, threshold=threshold)
        check(plane(res, 1) == plane(src, 1) and plane(res, 2) == plane(src, 2), "threshold %d leaves chroma unchanged" % threshold)

    expect_error(lambda: core.smoothuv.SmoothUV(src, radius=0), "radius 0 rejected", "radius")
    expect_error(lambda: core.smoothuv.SmoothUV(src, radius=8), "radius 8 rejected", "radius")
    expect_error(lambda: core.smoothuv.SmoothUV(src, threshold=451), "threshold 451 rejected", "threshold")
    expect_error(lambda: core.smoothuv.SmoothUV(src.resize.Point(format=vs.YUV420P10)), "10 bit rejected", "8 bit YUV")
    expect_error(lambda: core.smoothuv.SmoothUV(src.resize.Point(format=vs.RGB24, matrix_in_s="709")), "RGB rejected", "8 bit YUV")

    if FAILURES:
        raise SystemExit("%d check(s) failed" % len(FAILURES))
    print("all tests passed")


if __name__ == "__main__":
    main()
