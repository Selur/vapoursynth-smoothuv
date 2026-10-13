Description
===========

SmoothUV is a spatial derainbow filter.

The luma is returned unchanged.

x86 uses SSE2, other CPUs (e.g. arm64) a scalar version with identical
results.

This fork uses the VapourSynth API 4 and builds wheels for Windows x64,
Linux x86_64 and macOS arm64 (see `Fork changes`_).

This is a port of the SmoothUV filter from the SmoothUV Avisynth
plugin. The Avisynth plugin also includes the SSHiQ filter, which was
not ported.


RainbowSmooth is a script which adds edge detection to SmoothUV. It is
a port of the Avisynth function rainbow_smooth().


Usage
=====
::

    smoothuv.SmoothUV(clip clip, [int radius=3, int threshold=270, bint interlaced])


Parameters:
    *clip*
        A clip to process. It must have constant format and it must be
        8 bit YUV.

    *radius*
        Radius. Must be between 1 and 7.

        Larger values smooth more.

        Default: 3.

    *threshold*
        Threshold. Must be between 0 and 450.

        Larger values smooth more.

        Default: 270.

    *interlaced*
        Each frame's "_FieldBased" property is examined to determine if
        the frame should be considered interlaced. If the "_FieldBased"
        property is 0 or it doesn't exist, the frame is considered not
        interlaced.

        Set this parameter to override the automatic detection of
        interlaced frames.


::

    RainbowSmooth(clip, radius=3, lthresh=0, hthresh=220, mask="original")


Parameters:
    *clip*
        The clip to process.

    *radius*
        Radius passed to SmoothUV.

        Default: 3.

    *lthresh*, *hthresh*
        The low and the high smoothing thresholds. Use smaller values
        for safer processing. The masking is only used for hthresh,
        so if you set lthresh greater than hthresh lthresh will be the
        overall threshold and no masking will be used (fastest). But if
        you set lthresh=0 you disable the basic chroma smoothing and
        use only the chroma smoothing on edges.

        Default: lthresh=0, hthresh=220.

    *mask*
        Edge mask to use. It can be either a clip, or one of the
        following strings: "original", "prewitt", "sobel", "tcanny",
        "fast_sobel", "kirsch", "retinex_edgemask".

        "original" is the edge mask used by the original
        rainbow_smooth() Avisynth function.

        The latter three require kagefunc.py.

        Default: "original".

Compilation
===========

::

    meson setup build
    ninja -C build

Without VapourSynth headers on the system, the vendored API 4 headers in
``include/vapoursynth`` are used.

Wheel (installs into ``site-packages/vapoursynth/plugins``)::

    pip install .


Fork changes
============

- VapourSynth API 4 (``VapourSynthPluginInit2``), plugin version 1.1.
- Scalar code for CPUs without SSE2 (arm64), bit-identical to the SSE2 code.
- Right edge: when the chroma width is no multiple of 8, the last columns
  were averaged over a window left of the pixel or, if that window was
  empty, set to 0 (e.g. width 852: the last two chroma columns). These
  columns now use the window clamped at the image edge.
- Interlaced with an odd chroma height (e.g. 4:2:0 with 486 lines): the
  last chroma line was never written.
- Threshold 0 or 1 let no pixel pass the comparison and set the chroma
  to 0; the chroma is now returned unchanged.
- Everywhere else the output is identical to the original plugin.
- Meson build with meson-python wheels for Windows x64, Linux x86_64
  and macOS arm64; the autotools build was removed.


License
=======

GNU GPL, like the Avisynth plugin.
