#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if !defined(SMOOTHUV_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define SMOOTHUV_SSE2 1
#include <emmintrin.h>
#endif

#include <VapourSynth4.h>
#include <VSHelper4.h>


typedef struct SmoothUVData {
    VSNode *clip;
    const VSVideoInfo *vi;

    int radius;
    int threshold;
    bool interlaced;
    bool interlaced_exists;

    uint16_t divin[256];
} SmoothUVData;


// One output pixel: the same arithmetic as one 16 bit lane of the SSE2 code.
static inline uint8_t sum_pixel(const uint8_t *srcp, int center, const int stride,
                                const int x_first, const int x_count, const int height,
                                const int thres, const uint16_t *divinp) {
    unsigned sum = 0;
    unsigned count = 0;

    for (int y = 0; y < height; y++) {
        for (int x = x_first; x < x_first + x_count; x++) {
            int neighbour = srcp[x];
            int abs_diff = std::abs(center - neighbour);

            if (thres > abs_diff) {
                sum = std::min(sum + neighbour, 65535u);
                count++;
            }
        }

        srcp += stride;
    }

    sum = std::min(sum + ((count & 0xFFFF) >> 1), 65535u);

    unsigned value = (sum * divinp[count & 0xFF]) >> 16;
    int16_t packed = (int16_t)value;

    return (uint8_t)std::min(std::max((int)packed, 0), 255);
}


#ifdef SMOOTHUV_SSE2
static inline void sum_pixels(const uint8_t *srcp, uint8_t *dstp, const int stride,
                    const int diff, const int width, const int height,
                    const int threshold,
                    const uint16_t *divinp) {

    __m128i zeroes = _mm_setzero_si128();
    const __m128i thres = _mm_set1_epi16(threshold);

    __m128i sum = zeroes;
    __m128i count = zeroes;

    __m128i center_pixel = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)srcp),
                                             zeroes);

    srcp = srcp - diff;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            __m128i neighbour_pixel = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(srcp + x)),
                                                        zeroes);

            __m128i abs_diff = _mm_or_si128(_mm_subs_epu16(center_pixel, neighbour_pixel),
                                            _mm_subs_epu16(neighbour_pixel, center_pixel));

             // Absolute difference less than thres
            __m128i mask = _mm_cmpgt_epi16(thres, abs_diff);

            // Sum up the pixels that meet the criteria
            sum = _mm_adds_epu16(sum,
                                 _mm_and_si128(neighbour_pixel, mask));

            // Keep track of how many pixels are in the sum
            count = _mm_sub_epi16(count, mask);
        }

        srcp += stride;
    }

    sum = _mm_adds_epu16(sum,
                         _mm_srli_epi16(count, 1));

    alignas(16) uint16_t counts[8];
    alignas(16) uint16_t divisors[8];
    _mm_store_si128((__m128i *)counts, count);

    for (int i = 0; i < 8; i++)
        divisors[i] = divinp[counts[i]];

    __m128i divres = _mm_load_si128((const __m128i *)divisors);

    // Now multiply (divres/65536)
    sum = _mm_mulhi_epu16(sum, divres);
    sum = _mm_packus_epi16(sum, sum);
    _mm_storel_epi64((__m128i *)dstp, sum);
}
#else
// Eight pixels like the SSE2 code, lane by lane.
static inline void sum_pixels(const uint8_t *srcp, uint8_t *dstp, const int stride,
                    const int diff, const int width, const int height,
                    const int threshold,
                    const uint16_t *divinp) {
    for (int i = 0; i < 8; i++)
        dstp[i] = sum_pixel(srcp - diff + i, srcp[i], stride, 0, width, height, threshold, divinp);
}
#endif


// Processes eight pixels starting at column x; a block reaching past the last column is done pixel by pixel.
static inline void smooth_block(const uint8_t *srcp, uint8_t *dstp, int stride, int w,
                                int x, int radius, int offset, int yn,
                                int thres, const uint16_t *divin) {
    if (x + 8 > w) {
        for (int px = x; px < w; px++) {
            int first = std::min(px, radius);
            int last = std::min(px + radius, w - 1);
            dstp[px] = sum_pixel(srcp + px - offset, srcp[px], stride,
                                 -first, last - px + first + 1, yn, thres, divin);
        }
        return;
    }

    int x0 = (x < radius) ? x : radius;

    int xn = (x + 7 + radius < w - 1) ? x0 + radius + 1
                                      : x0 + w - x - 7;

    sum_pixels(srcp + x, dstp + x,
               stride,
               offset + x0,
               xn, yn,
               thres,
               divin);
}


template <bool interlaced>
static void smoothN(int radius,
                    const uint8_t* origsrc, uint8_t* origdst,
                    int stride, int w, int h,
                    const int threshold,
                    const uint16_t* divin) {
    const uint8_t *srcp = origsrc;
    const uint8_t *srcp2 = origsrc + stride;
    uint8_t *dstp = origdst;
    uint8_t *dstp2 = origdst + stride;

    const int SqrtTsquared = (int)sqrt((threshold * threshold) / 3);

    // Below threshold 2 no pixel, not even the centre, passes the comparison: nothing to smooth.
    if (SqrtTsquared == 0) {
        vsh::bitblt(origdst, stride, origsrc, stride, w, h);
        return;
    }

    int h2 = h;

    if (interlaced) {
        stride *= 2;
        h2 >>= 1;
    }

    for (int y = 0; y < h2; y++) {
        int y0 = (y < radius) ? y : radius;

        int yn = (y < h2 - radius) ? y0 + radius + 1
                                   : y0 + (h2 - y);

        if (interlaced)
            yn--;

        int offset = y0 * stride;

        for (int x = 0; x < w; x += 8) {
            smooth_block(srcp, dstp, stride, w, x, radius, offset, yn, SqrtTsquared, divin);

            if (interlaced)
                smooth_block(srcp2, dstp2, stride, w, x, radius, offset, yn, SqrtTsquared, divin);
        }

        dstp += stride;
        srcp += stride;
        dstp2 += stride;
        srcp2 += stride;
    }

    // Odd plane height (4:2:0 chroma of e.g. 486 lines): the last line belongs to the first field.
    if (interlaced && h % 2) {
        int yn = radius;

        int offset = radius * stride;

        for (int x = 0; x < w; x += 8)
            smooth_block(srcp, dstp, stride, w, x, radius, offset, yn, SqrtTsquared, divin);
    }
}


static const VSFrame *VS_CC smoothUVGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;

    const SmoothUVData *d = (const SmoothUVData *)instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->clip, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *src = vsapi->getFrameFilter(n, d->clip, frameCtx);

        int width = vsapi->getFrameWidth(src, 0);
        int height = vsapi->getFrameHeight(src, 0);

        // Reuse the luma plane, allocate memory for the chroma planes.
        const VSFrame *plane_src[3] = { src, nullptr, nullptr };
        int planes[3] = { 0 };

        VSFrame *dst = vsapi->newVideoFrame2(&d->vi->format, width, height, plane_src, planes, src, core);

        const VSMap *props = vsapi->getFramePropertiesRO(src);
        int err;
        int64_t field_based = vsapi->mapGetInt(props, "_FieldBased", 0, &err);

        bool interlaced = field_based == 1 || field_based == 2;
        if (d->interlaced_exists)
            interlaced = d->interlaced;

        for (int plane = 1; plane < 3; plane++) {
            const uint8_t *srcp = vsapi->getReadPtr(src, plane);
            uint8_t *dstp = vsapi->getWritePtr(dst, plane);

            ptrdiff_t stride = vsapi->getStride(src, plane);
            width = vsapi->getFrameWidth(src, plane);
            height = vsapi->getFrameHeight(src, plane);

            (interlaced ? smoothN<true>
                        : smoothN<false>)(d->radius, srcp, dstp, (int)stride, width, height, d->threshold, d->divin);
        }

        vsapi->freeFrame(src);

        return dst;
    }

    return nullptr;
}


static void VS_CC smoothUVFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;

    SmoothUVData *d = (SmoothUVData *)instanceData;

    vsapi->freeNode(d->clip);

    free(d);
}


static void VS_CC smoothUVCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    SmoothUVData d;
    memset(&d, 0, sizeof(d));

    int err;

    d.radius = vsh::int64ToIntS(vsapi->mapGetInt(in, "radius", 0, &err));
    if (err)
        d.radius = 3;

    d.threshold = vsh::int64ToIntS(vsapi->mapGetInt(in, "threshold", 0, &err));
    if (err)
        d.threshold = 270;

    d.interlaced = !!vsapi->mapGetInt(in, "interlaced", 0, &err);
    d.interlaced_exists = !err;


    if (d.radius < 1 || d.radius > 7) {
        vsapi->mapSetError(out, "SmoothUV: radius must be between 1 and 7 (inclusive).");
        return;
    }

    if (d.threshold < 0 || d.threshold > 450) {
        vsapi->mapSetError(out, "SmoothUV: threshold must be between 0 and 450 (inclusive).");
        return;
    }


    d.clip = vsapi->mapGetNode(in, "clip", 0, nullptr);
    d.vi = vsapi->getVideoInfo(d.clip);

    if (!vsh::isConstantVideoFormat(d.vi) ||
        d.vi->format.sampleType != stInteger ||
        d.vi->format.bitsPerSample != 8 ||
        d.vi->format.colorFamily != cfYUV) {
        vsapi->mapSetError(out, "SmoothUV: only 8 bit YUV with constant format supported.");
        vsapi->freeNode(d.clip);
        return;
    }


    for (int i = 1; i < 256; i++)
        d.divin[i] = (uint16_t)std::min((int)(65536.0 / i + 0.5), 65535);


    SmoothUVData *data = (SmoothUVData *)malloc(sizeof(SmoothUVData));
    *data = d;

    VSFilterDependency deps[] = { { data->clip, rpStrictSpatial } };
    vsapi->createVideoFilter(out, "SmoothUV", data->vi, smoothUVGetFrame, smoothUVFree, fmParallel, deps, 1, data, core);
}


VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.nodame.smoothuv", "smoothuv", "SmoothUV is a spatial derainbow filter", VS_MAKE_VERSION(1, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("SmoothUV",
                             "clip:vnode;"
                             "radius:int:opt;"
                             "threshold:int:opt;"
                             "interlaced:int:opt;",
                             "clip:vnode;",
                             smoothUVCreate, nullptr, plugin);
}
