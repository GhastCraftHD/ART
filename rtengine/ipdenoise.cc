/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 Structure of the algorithm:

 1. Compute an initial denoise of the image via undecimated wavelet transform
 and universal thresholding modulated by user input.
 2. Decompose the residual image into TSxTS size tiles, shifting by 'offset'
 each step (so roughly each pixel is in (TS/offset)^2 tiles); Discrete Cosine
 transform the tiles.
 3. Filter the DCT data to pick out patterns missed by the wavelet denoise
 4. Inverse DCT the denoised tile data and combine the tiles into a denoised
 output image.
 5. Optional final smoothing via guided filter (for chrominance) and
 non-local means (for luminance), in linear RGB space
 */

#include "ipdenoise.h"
#include "imagesource.h"
#include "improcfun.h"
#include "mytime.h"
#include "iccstore.h"
#include "rt_algo.h"
#include "wavelet.h"

/* RGB_denoise's body is split into eight named phases sharing one
 * DenoiseContext (see the comment above its definition in namespace denoise
 * below), built by denoisePrepare into the wider DenoisePrep.  DenoisePrep and
 * DctWorkspace are deliberately local to this TU -- nothing outside needs
 * them, and DctWorkspace could not leave anyway, since it is built on the
 * TS/offset/blkrad macros that are #undef'd below. */
#include "../rtgui/threadutils.h"
#include "LUT.h"
#include "array2D.h"
#include "boxblur.h"
#include "gauss.h"
#include "guidedfilter.h"
#include "iccmatrices.h"
#include "median.h"
#include "opthelper.h"
#include "rescale.h"
#include "rt_math.h"
#include "rtengine.h"
#include "sleef.h"

#include <cmath>
#include <cstdlib>
#include <fftw3.h>
#include <iostream>
#include <memory>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "StopWatch.h"

namespace rtengine {

extern const Settings *settings;
using namespace procparams;

namespace denoise {

/* How many wavelet levels to decompose to.  Stronger chroma sliders and the
 * aggressive (QUALITY_HIGH) mode ask for more levels; the image's short side
 * and the preview scale cap it.  8 is the hard ceiling (wavelet.h's
 * maxlevels). */
enum nrquality { QUALITY_STANDARD, QUALITY_HIGH };

/* ---------------------------------------------------------------------------
 * RGB_denoise's phases
 *
 * RGB_denoise's body decomposes into eight phases:
 *
 *   1 fill        RGB -> L/a/b through the gamma LUTs  [denoiseFill]
 *   2 decompose   wavelet decomposition of L, a, b
 *   3 mad         madL[level][dir] noise estimate from L's detail bands
 *   4 shrink-ab   chroma shrinkage (cross-channel: reads L's coefficients)
 *   5 shrink-l    luma shrinkage
 *   6 reconstruct wavelet reconstruction of L, a, b
 *   7 dct         sliding-window block DCT detail recovery
 *                 [buildDetailMask + detailRecovery]
 *   8 out         L/a/b -> RGB through the inverse gamma LUT  [denoiseOutput]
 *
 * DenoiseContext carries what the phases share, so each phase reads as one
 * operation on a named input rather than as a slice of a long function.  It is
 * built by denoisePrepare, which fills the wider DenoisePrep around it; the
 * two are deliberately local to this TU -- nothing outside needs them.
 * ------------------------------------------------------------------------ */
struct DenoiseContext {
    /* Geometry.  W2 is the width of the quarter-resolution noisevar maps,
     * and is exactly the width of one wavelet level plane. */
    int W, H, W2;
    double scale;

    /* Mode flags. */
    bool lab_mode;
    bool useNoiseCCurve;

    /* Noise strengths.  noisevarL is the flat luma strength used when there
     * is no luma noise curve; noisevarab_r/b are the per-channel chroma
     * strengths derived from the sliders. */
    float noisevarL;
    float noisevarab_r, noisevarab_b;

    /* Working-space matrices, forward and inverse. */
    const float (*wpi)[3];
    const float (*wpi_inverse)[3];

    /* Gamma.  The LUTs cover [0, 65535]; outside that range the phases fall
     * back to the closed form, which is what applyGamma/applyIGamma below
     * encapsulate (they were lambdas inside RGB_denoise). */
    float gam;
    float gamthresh, gamslope;
    float igamthresh, igamslope;
    const LUTf *gamcurve;
    const LUTf *igamcurve;

    /* Quarter-resolution per-pixel noise maps, W2 x ((H+1)/2). */
    float *noisevarlum;
    float *noisevarchrom;

    /* Nested OpenMP thread count for the phases' inner parallel regions,
     * computed once in denoisePrepare from the host's processor count and
     * options.rgbDenoiseThreadLimit. */
    int denoiseNestedLevels;

    float applyGamma(float v) const
    {
        if (gam > 1.f && v > 0.f) {
            return v < 65535.f
                       ? (*gamcurve)[v]
                       : (Color::gammaf(v / 65535.f, gam, gamthresh, gamslope) *
                          65535.f);
        }
        return v;
    }

    /* Note the 65536 here against applyGamma's 65535.  That asymmetry is in
     * the original code and is preserved deliberately: for v in
     * [65535, 65536) the forward transform takes the closed form while the
     * inverse takes the LUT (which clamps internally, so it is a defined if
     * slightly different value).  Not worth "fixing" -- it would change
     * output for no benefit. */
    float applyIGamma(float v) const
    {
        if (gam > 1.f && v > 0.f) {
            return v < 65536.f ? (*igamcurve)[v]
                               : (Color::gammaf(v / 65535.f, 1.f / gam,
                                                igamthresh, igamslope) *
                                  65535.f);
        }
        return v;
    }
};

float MadRgb(float *DataList, const int datalen)
{
    if (datalen <= 1) { // Avoid possible buffer underrun
        return 0;
    }

    // computes Median Absolute Deviation
    // DataList values should mostly have abs val < 65536 because we are in RGB
    // mode
    int *histo = new int[65536];

    for (int i = 0; i < 65536; ++i) {
        histo[i] = 0;
    }

    // calculate histogram of absolute values of wavelet coeffs
    int i;

    for (i = 0; i < datalen; ++i) {
        histo[min(65535, static_cast<int>(abs(DataList[i])))]++;
    }

    // find median of histogram
    int median = 0, count = 0;

    while (count < datalen / 2) {
        count += histo[median];
        ++median;
    }

    int count_ = count - histo[median - 1];

    // interpolate
    delete[] histo;
    return (((median - 1) +
             (datalen / 2 - count_) / (static_cast<float>(count - count_))) /
            0.6745);
}

void ShrinkAllL(double scale, wavelet_decomposition &WaveletCoeffs_L,
                float **buffer, int level, int dir, float *noisevarlum,
                float *madL, float *vari, int edge)

{
    // simple wavelet shrinkage
    const float eps = 0.01f;

    float *sfave = buffer[0] + 32;
    float *sfaved = buffer[1] + 64;
    float *blurBuffer = buffer[2] + 96;

    int W_L = WaveletCoeffs_L.level_W(level);
    int H_L = WaveletCoeffs_L.level_H(level);

    float **WavCoeffs_L = WaveletCoeffs_L.level_coeffs(level);
    //      printf("OK lev=%d\n",level);
    float mad_L = madL[dir - 1];

    if (edge == 1 && vari) {
        noisevarlum =
            blurBuffer; // we need one buffer, but fortunately we don't have to
                        // allocate a new one because we can use blurBuffer

        for (int i = 0; i < W_L * H_L; ++i) {
            noisevarlum[i] = vari[level];
        }
    }

    float levelFactor = mad_L * 5.f / static_cast<float>(level + 1);
#ifdef ART_SIMD
    __m128 magv;
    __m128 levelFactorv = _mm_set1_ps(levelFactor);
    __m128 mad_Lv;
    __m128 ninev = _mm_set1_ps(9.0f);
    __m128 epsv = _mm_set1_ps(eps);
    int i;

    for (i = 0; i < W_L * H_L - 3; i += 4) {
        mad_Lv = LVFU(noisevarlum[i]) * levelFactorv;
        magv = SQRV(LVFU(WavCoeffs_L[dir][i]));
        _mm_storeu_ps(
            &sfave[i],
            magv / (magv + mad_Lv * xexpf(-magv / (ninev * mad_Lv)) + epsv));
    }

    // few remaining pixels
    for (; i < W_L * H_L; ++i) {
        float mag = SQR(WavCoeffs_L[dir][i]);
        sfave[i] = mag / (mag +
                          levelFactor * noisevarlum[i] *
                              xexpf(-mag / (9 * levelFactor * noisevarlum[i])) +
                          eps);
    }

#else

    for (int i = 0; i < W_L * H_L; ++i) {

        float mag = SQR(WavCoeffs_L[dir][i]);
        float shrinkfactor =
            mag / (mag +
                   levelFactor * noisevarlum[i] *
                       xexpf(-mag / (9 * levelFactor * noisevarlum[i])) +
                   eps);
        sfave[i] = shrinkfactor;
    }

#endif
    const int blur_rad = max(1, int((level + 2) / scale));
    boxblur(sfave, sfaved, blurBuffer, blur_rad, blur_rad, W_L,
            H_L); // increase smoothness by locally averaging shrinkage

#ifdef ART_SIMD
    __m128 sfv;

    for (i = 0; i < W_L * H_L - 3; i += 4) {
        sfv = LVFU(sfave[i]);
        // use smoothed shrinkage unless local shrinkage is much less
        _mm_storeu_ps(&WavCoeffs_L[dir][i],
                      _mm_loadu_ps(&WavCoeffs_L[dir][i]) *
                          (SQRV(LVFU(sfaved[i])) + SQRV(sfv)) /
                          (LVFU(sfaved[i]) + sfv + epsv));
    }

    // few remaining pixels
    for (; i < W_L * H_L; ++i) {
        float sf = sfave[i];

        // use smoothed shrinkage unless local shrinkage is much less
        WavCoeffs_L[dir][i] *=
            (SQR(sfaved[i]) + SQR(sf)) / (sfaved[i] + sf + eps);
    } // now luminance coefficients are denoised

#else

    for (int i = 0; i < W_L * H_L; ++i) {
        float sf = sfave[i];

        // use smoothed shrinkage unless local shrinkage is much less
        WavCoeffs_L[dir][i] *=
            (SQR(sfaved[i]) + SQR(sf)) / (sfaved[i] + sf + eps);

    } // now luminance coefficients are denoised

#endif
}

void ShrinkAllAB(double scale, wavelet_decomposition &WaveletCoeffs_L,
                 wavelet_decomposition &WaveletCoeffs_ab, float **buffer,
                 int level, int dir, float *noisevarchrom, float noisevar_ab,
                 const bool useNoiseCCurve, bool autoch, float *madL,
                 float *madaab = nullptr, bool madCalculated = false)

{
    // simple wavelet shrinkage
    const float eps = 0.01f;

    if (autoch && noisevar_ab <= 0.001f) {
        noisevar_ab = 0.02f;
    }

    float *sfaveab = buffer[0] + 32;
    float *sfaveabd = buffer[1] + 64;
    float *blurBuffer = buffer[2] + 96;

    int W_ab = WaveletCoeffs_ab.level_W(level);
    int H_ab = WaveletCoeffs_ab.level_H(level);

    float **WavCoeffs_L = WaveletCoeffs_L.level_coeffs(level);
    float **WavCoeffs_ab = WaveletCoeffs_ab.level_coeffs(level);

    float madab;
    float mad_L = madL[dir - 1];

    if (madCalculated) {
        madab = madaab[dir - 1];
    } else {
        madab = SQR(MadRgb(WavCoeffs_ab[dir], W_ab * H_ab));
    }

    if (noisevar_ab > 0.001f) {
        madab = useNoiseCCurve ? madab : madab * noisevar_ab;
#ifdef ART_SIMD
        __m128 onev = _mm_set1_ps(1.f);
        __m128 mad_abrv = _mm_set1_ps(madab);

        __m128 rmadLm9v = onev / _mm_set1_ps(mad_L * 9.f);
        __m128 mad_abv;
        __m128 mag_Lv, mag_abv;
        int coeffloc_ab;

        for (coeffloc_ab = 0; coeffloc_ab < H_ab * W_ab - 3; coeffloc_ab += 4) {
            mad_abv = LVFU(noisevarchrom[coeffloc_ab]) * mad_abrv;

            mag_Lv = LVFU(WavCoeffs_L[dir][coeffloc_ab]);
            mag_abv = SQRV(LVFU(WavCoeffs_ab[dir][coeffloc_ab]));
            mag_Lv = (SQRV(mag_Lv))*rmadLm9v;
            _mm_storeu_ps(&sfaveab[coeffloc_ab],
                          (onev - xexpf(-(mag_abv / mad_abv) - (mag_Lv))));
        }

        // few remaining pixels
        for (; coeffloc_ab < H_ab * W_ab; ++coeffloc_ab) {
            float mag_L = SQR(WavCoeffs_L[dir][coeffloc_ab]);
            float mag_ab = SQR(WavCoeffs_ab[dir][coeffloc_ab]);
            sfaveab[coeffloc_ab] =
                (1.f - xexpf(-(mag_ab / (noisevarchrom[coeffloc_ab] * madab)) -
                             (mag_L / (9.f * mad_L))));
        } // now chrominance coefficients are denoised

#else

        for (int i = 0; i < H_ab; ++i) {
            for (int j = 0; j < W_ab; ++j) {
                int coeffloc_ab = i * W_ab + j;
                float mag_L = SQR(WavCoeffs_L[dir][coeffloc_ab]);
                float mag_ab = SQR(WavCoeffs_ab[dir][coeffloc_ab]);
                sfaveab[coeffloc_ab] =
                    (1.f -
                     xexpf(-(mag_ab / (noisevarchrom[coeffloc_ab] * madab)) -
                           (mag_L / (9.f * mad_L))));
            }
        } // now chrominance coefficients are denoised

#endif

        const int blur_rad = max(1, int((level + 2) / scale));
        boxblur(sfaveab, sfaveabd, blurBuffer, blur_rad, blur_rad, W_ab,
                H_ab); // increase smoothness by locally averaging shrinkage
#ifdef ART_SIMD
        __m128 epsv = _mm_set1_ps(eps);
        __m128 sfabv;
        __m128 sfaveabv;

        for (coeffloc_ab = 0; coeffloc_ab < H_ab * W_ab - 3; coeffloc_ab += 4) {
            sfabv = LVFU(sfaveab[coeffloc_ab]);
            sfaveabv = LVFU(sfaveabd[coeffloc_ab]);

            // use smoothed shrinkage unless local shrinkage is much less
            _mm_storeu_ps(&WavCoeffs_ab[dir][coeffloc_ab],
                          LVFU(WavCoeffs_ab[dir][coeffloc_ab]) *
                              (SQRV(sfaveabv) + SQRV(sfabv)) /
                              (sfaveabv + sfabv + epsv));
        }

        // few remaining pixels
        for (; coeffloc_ab < H_ab * W_ab; ++coeffloc_ab) {
            // modification Jacques feb 2013
            float sfab = sfaveab[coeffloc_ab];

            // use smoothed shrinkage unless local shrinkage is much less
            WavCoeffs_ab[dir][coeffloc_ab] *=
                (SQR(sfaveabd[coeffloc_ab]) + SQR(sfab)) /
                (sfaveabd[coeffloc_ab] + sfab + eps);
        } // now chrominance coefficients are denoised

#else

        for (int i = 0; i < H_ab; ++i) {
            for (int j = 0; j < W_ab; ++j) {
                int coeffloc_ab = i * W_ab + j;
                float sfab = sfaveab[coeffloc_ab];

                // use smoothed shrinkage unless local shrinkage is much less
                WavCoeffs_ab[dir][coeffloc_ab] *=
                    (SQR(sfaveabd[coeffloc_ab]) + SQR(sfab)) /
                    (sfaveabd[coeffloc_ab] + sfab + eps);
            } // now chrominance coefficients are denoised
        }

#endif
    }
}

bool WaveletDenoiseAll_BiShrinkL(double scale,
                                 wavelet_decomposition &WaveletCoeffs_L,
                                 float *noisevarlum, float madL[8][3],
                                 int denoiseNestedLevels)
{
    int maxlvl = min(WaveletCoeffs_L.maxlevel(), 5);
    const float eps = 0.01f;

    int maxWL = 0, maxHL = 0;

    for (int lvl = 0; lvl < maxlvl; ++lvl) {
        if (WaveletCoeffs_L.level_W(lvl) > maxWL) {
            maxWL = WaveletCoeffs_L.level_W(lvl);
        }

        if (WaveletCoeffs_L.level_H(lvl) > maxHL) {
            maxHL = WaveletCoeffs_L.level_H(lvl);
        }
    }

#ifdef _OPENMP
#pragma omp parallel num_threads(                                              \
        denoiseNestedLevels) if (denoiseNestedLevels > 1)
#endif
    {
        float *buffer[3];
        buffer[0] = new /*(std::nothrow)*/ float[maxWL * maxHL + 32];
        buffer[1] = new /*(std::nothrow)*/ float[maxWL * maxHL + 64];
        buffer[2] = new /*(std::nothrow)*/ float[maxWL * maxHL + 96];

        // if (buffer[0] == nullptr || buffer[1] == nullptr || buffer[2] ==
        // nullptr) {

#ifdef _OPENMP
#pragma omp for schedule(dynamic) collapse(2)
#endif

        for (int lvl = maxlvl - 1; lvl >= 0;
             lvl--) { // for levels less than max, use level diff to make
                      // edge mask
            for (int dir = 1; dir < 4; ++dir) {
                int Wlvl_L = WaveletCoeffs_L.level_W(lvl);
                int Hlvl_L = WaveletCoeffs_L.level_H(lvl);

                float **WavCoeffs_L = WaveletCoeffs_L.level_coeffs(lvl);

                if (lvl == maxlvl - 1) {
                    int edge = 0;
                    ShrinkAllL(scale, WaveletCoeffs_L, buffer, lvl, dir,
                               noisevarlum, madL[lvl], nullptr, edge);
                } else {
                    // simple wavelet shrinkage
                    float *sfave = buffer[0] + 32;
                    float *sfaved = buffer[2] + 96;
                    float *blurBuffer = buffer[1] + 64;

                    float mad_Lr = madL[lvl][dir - 1];

                    float levelFactor = mad_Lr * 5.f / (lvl + 1);
#ifdef ART_SIMD
                    __m128 mad_Lv;
                    __m128 ninev = _mm_set1_ps(9.0f);
                    __m128 epsv = _mm_set1_ps(eps);
                    __m128 mag_Lv;
                    __m128 levelFactorv = _mm_set1_ps(levelFactor);
                    int coeffloc_L;

                    for (coeffloc_L = 0; coeffloc_L < Hlvl_L * Wlvl_L - 3;
                         coeffloc_L += 4) {
                        mad_Lv =
                            LVFU(noisevarlum[coeffloc_L]) * levelFactorv;
                        mag_Lv = SQRV(LVFU(WavCoeffs_L[dir][coeffloc_L]));
                        _mm_storeu_ps(
                            &sfave[coeffloc_L],
                            mag_Lv / (mag_Lv +
                                      mad_Lv * xexpf(-mag_Lv /
                                                     (mad_Lv * ninev)) +
                                      epsv));
                    }

                    for (; coeffloc_L < Hlvl_L * Wlvl_L; ++coeffloc_L) {
                        float mag_L = SQR(WavCoeffs_L[dir][coeffloc_L]);
                        sfave[coeffloc_L] =
                            mag_L /
                            (mag_L +
                             levelFactor * noisevarlum[coeffloc_L] *
                                 xexpf(-mag_L / (9.f * levelFactor *
                                                 noisevarlum[coeffloc_L])) +
                             eps);
                    }

#else

                    for (int i = 0; i < Hlvl_L; ++i) {
                        for (int j = 0; j < Wlvl_L; ++j) {

                            int coeffloc_L = i * Wlvl_L + j;
                            float mag_L = SQR(WavCoeffs_L[dir][coeffloc_L]);
                            sfave[coeffloc_L] =
                                mag_L /
                                (mag_L +
                                 levelFactor * noisevarlum[coeffloc_L] *
                                     xexpf(-mag_L /
                                           (9.f * levelFactor *
                                            noisevarlum[coeffloc_L])) +
                                 eps);
                        }
                    }

#endif
                    const int blur_rad = max(1, int((lvl + 2) / scale));
                    boxblur(sfave, sfaved, blurBuffer, blur_rad, blur_rad,
                            Wlvl_L, Hlvl_L); // increase smoothness by
                                             // locally averaging shrinkage
#ifdef ART_SIMD
                    __m128 sfavev;
                    __m128 sf_Lv;

                    for (coeffloc_L = 0; coeffloc_L < Hlvl_L * Wlvl_L - 3;
                         coeffloc_L += 4) {
                        sfavev = LVFU(sfaved[coeffloc_L]);
                        sf_Lv = LVFU(sfave[coeffloc_L]);
                        _mm_storeu_ps(&WavCoeffs_L[dir][coeffloc_L],
                                      LVFU(WavCoeffs_L[dir][coeffloc_L]) *
                                          (SQRV(sfavev) + SQRV(sf_Lv)) /
                                          (sfavev + sf_Lv + epsv));
                        // use smoothed shrinkage unless local shrinkage is
                        // much less
                    }

                    // few remaining pixels
                    for (; coeffloc_L < Hlvl_L * Wlvl_L; ++coeffloc_L) {
                        float sf_L = sfave[coeffloc_L];
                        // use smoothed shrinkage unless local shrinkage is
                        // much less
                        WavCoeffs_L[dir][coeffloc_L] *=
                            (SQR(sfaved[coeffloc_L]) + SQR(sf_L)) /
                            (sfaved[coeffloc_L] + sf_L + eps);
                    } // now luminance coeffs are denoised

#else

                    for (int i = 0; i < Hlvl_L; ++i) {
                        for (int j = 0; j < Wlvl_L; ++j) {
                            int coeffloc_L = i * Wlvl_L + j;
                            float sf_L = sfave[coeffloc_L];
                            // use smoothed shrinkage unless local shrinkage
                            // is much less
                            WavCoeffs_L[dir][coeffloc_L] *=
                                (SQR(sfaved[coeffloc_L]) + SQR(sf_L)) /
                                (sfaved[coeffloc_L] + sf_L + eps);
                        } // now luminance coeffs are denoised
                    }

#endif
                }
            }
        }

        for (int i = 2; i >= 0; i--) {
            if (buffer[i] != nullptr) {
                delete[] buffer[i];
            }
        }
    }
    return true;
}

bool WaveletDenoiseAll_BiShrinkAB(double scale,
                                  wavelet_decomposition &WaveletCoeffs_L,
                                  wavelet_decomposition &WaveletCoeffs_ab,
                                  float *noisevarchrom, float madL[8][3],
                                  float noisevar_ab, const bool useNoiseCCurve,
                                  bool autoch, int denoiseNestedLevels)
{
    int maxlvl = WaveletCoeffs_L.maxlevel();

    if (autoch && noisevar_ab <= 0.001f) {
        noisevar_ab = 0.02f;
    }

    float madab[8][3];

    int maxWL = 0, maxHL = 0;

    for (int lvl = 0; lvl < maxlvl; ++lvl) {
        if (WaveletCoeffs_L.level_W(lvl) > maxWL) {
            maxWL = WaveletCoeffs_L.level_W(lvl);
        }

        if (WaveletCoeffs_L.level_H(lvl) > maxHL) {
            maxHL = WaveletCoeffs_L.level_H(lvl);
        }
    }

#ifdef _OPENMP
#pragma omp parallel num_threads(                                              \
        denoiseNestedLevels) if (denoiseNestedLevels > 1)
#endif
    {
        float *buffer[3];
        buffer[0] = new /*(std::nothrow)*/ float[maxWL * maxHL + 32];
        buffer[1] = new /*(std::nothrow)*/ float[maxWL * maxHL + 64];
        buffer[2] = new /*(std::nothrow)*/ float[maxWL * maxHL + 96];

        // if (buffer[0] == nullptr || buffer[1] == nullptr || buffer[2] ==
        // nullptr) {

#ifdef _OPENMP
#pragma omp for schedule(dynamic) collapse(2)
#endif

        for (int lvl = 0; lvl < maxlvl; ++lvl) {
            for (int dir = 1; dir < 4; ++dir) {
                // compute median absolute deviation (MAD) of detail
                // coefficients as robust noise estimator
                int Wlvl_ab = WaveletCoeffs_ab.level_W(lvl);
                int Hlvl_ab = WaveletCoeffs_ab.level_H(lvl);
                float **WavCoeffs_ab = WaveletCoeffs_ab.level_coeffs(lvl);
                madab[lvl][dir - 1] =
                    SQR(MadRgb(WavCoeffs_ab[dir], Wlvl_ab * Hlvl_ab));
            }
        }

#ifdef _OPENMP
#pragma omp for schedule(dynamic) collapse(2)
#endif

        for (int lvl = maxlvl - 1; lvl >= 0;
             lvl--) { // for levels less than max, use level diff to make
                      // edge mask
            for (int dir = 1; dir < 4; ++dir) {
                int Wlvl_ab = WaveletCoeffs_ab.level_W(lvl);
                int Hlvl_ab = WaveletCoeffs_ab.level_H(lvl);

                float **WavCoeffs_L = WaveletCoeffs_L.level_coeffs(lvl);
                float **WavCoeffs_ab = WaveletCoeffs_ab.level_coeffs(lvl);

                if (lvl == maxlvl - 1) {
                    ShrinkAllAB(scale, WaveletCoeffs_L, WaveletCoeffs_ab,
                                buffer, lvl, dir, noisevarchrom,
                                noisevar_ab, useNoiseCCurve, autoch,
                                madL[lvl], madab[lvl], true);
                } else {
                    // simple wavelet shrinkage

                    float mad_Lr = madL[lvl][dir - 1];
                    float mad_abr =
                        useNoiseCCurve
                            ? noisevar_ab * madab[lvl][dir - 1]
                            : SQR(noisevar_ab) * madab[lvl][dir - 1];

                    if (noisevar_ab > 0.001f) {

#ifdef ART_SIMD
                        __m128 onev = _mm_set1_ps(1.f);
                        __m128 mad_abrv = _mm_set1_ps(mad_abr);
                        __m128 rmad_Lm9v = onev / _mm_set1_ps(mad_Lr * 9.f);
                        __m128 mad_abv;
                        __m128 mag_Lv, mag_abv;
                        __m128 tempabv;
                        int coeffloc_ab;

                        for (coeffloc_ab = 0;
                             coeffloc_ab < Hlvl_ab * Wlvl_ab - 3;
                             coeffloc_ab += 4) {
                            mad_abv =
                                LVFU(noisevarchrom[coeffloc_ab]) * mad_abrv;

                            tempabv = LVFU(WavCoeffs_ab[dir][coeffloc_ab]);
                            mag_Lv = LVFU(WavCoeffs_L[dir][coeffloc_ab]);
                            mag_abv = SQRV(tempabv);
                            mag_Lv = SQRV(mag_Lv) * rmad_Lm9v;
                            _mm_storeu_ps(
                                &WavCoeffs_ab[dir][coeffloc_ab],
                                tempabv * SQRV((onev -
                                                xexpf(-(mag_abv / mad_abv) -
                                                      (mag_Lv)))));
                        }

                        // few remaining pixels
                        for (; coeffloc_ab < Hlvl_ab * Wlvl_ab;
                             ++coeffloc_ab) {
                            float mag_L =
                                SQR(WavCoeffs_L[dir][coeffloc_ab]);
                            float mag_ab =
                                SQR(WavCoeffs_ab[dir][coeffloc_ab]);
                            WavCoeffs_ab[dir][coeffloc_ab] *= SQR(
                                1.f -
                                xexpf(
                                    -(mag_ab / (noisevarchrom[coeffloc_ab] *
                                                mad_abr)) -
                                    (mag_L /
                                     (9.f * mad_Lr))) /*satfactor_a*/);
                        } // now chrominance coefficients are denoised

#else

                        for (int i = 0; i < Hlvl_ab; ++i) {
                            for (int j = 0; j < Wlvl_ab; ++j) {
                                int coeffloc_ab = i * Wlvl_ab + j;

                                float mag_L =
                                    SQR(WavCoeffs_L[dir][coeffloc_ab]);
                                float mag_ab =
                                    SQR(WavCoeffs_ab[dir][coeffloc_ab]);

                                WavCoeffs_ab[dir][coeffloc_ab] *= SQR(
                                    1.f -
                                    xexpf(
                                        -(mag_ab /
                                          (noisevarchrom[coeffloc_ab] *
                                           mad_abr)) -
                                        (mag_L /
                                         (9.f * mad_Lr))) /*satfactor_a*/);
                            }
                        } // now chrominance coefficients are denoised

#endif
                    }
                }
            }
        }

        for (int i = 2; i >= 0; i--) {
            if (buffer[i] != nullptr) {
                delete[] buffer[i];
            }
        }
    }
    return true;
}

bool WaveletDenoiseAllL(double scale, wavelet_decomposition &WaveletCoeffs_L,
                        float *noisevarlum, float madL[8][3], float *vari,
                        int edge, int denoiseNestedLevels) // mod JD

{

    int maxlvl = min(WaveletCoeffs_L.maxlevel(), 5);

    if (edge == 1) {
        maxlvl = 4; // for refine denoise edge wavelet
    }

    int maxWL = 0, maxHL = 0;

    for (int lvl = 0; lvl < maxlvl; ++lvl) {
        if (WaveletCoeffs_L.level_W(lvl) > maxWL) {
            maxWL = WaveletCoeffs_L.level_W(lvl);
        }

        if (WaveletCoeffs_L.level_H(lvl) > maxHL) {
            maxHL = WaveletCoeffs_L.level_H(lvl);
        }
    }

#ifdef _OPENMP
#pragma omp parallel num_threads(                                              \
        denoiseNestedLevels) if (denoiseNestedLevels > 1)
#endif
    {
        float *buffer[4];
        buffer[0] = new /*(std::nothrow)*/ float[maxWL * maxHL + 32];
        buffer[1] = new /*(std::nothrow)*/ float[maxWL * maxHL + 64];
        buffer[2] = new /*(std::nothrow)*/ float[maxWL * maxHL + 96];
        buffer[3] = new /*(std::nothrow)*/ float[maxWL * maxHL + 128];

        // if (buffer[0] == nullptr || buffer[1] == nullptr || buffer[2] ==
        // nullptr || buffer[3] == nullptr) {

#ifdef _OPENMP
#pragma omp for schedule(dynamic) collapse(2)
#endif

        for (int lvl = 0; lvl < maxlvl; ++lvl) {
            for (int dir = 1; dir < 4; ++dir) {
                ShrinkAllL(scale, WaveletCoeffs_L, buffer, lvl, dir,
                           noisevarlum, madL[lvl], vari, edge);
            }
        }

        for (int i = 3; i >= 0; i--) {
            if (buffer[i] != nullptr) {
                delete[] buffer[i];
            }
        }
    }
    return true;
}

bool WaveletDenoiseAllAB(double scale, wavelet_decomposition &WaveletCoeffs_L,
                         wavelet_decomposition &WaveletCoeffs_ab,
                         float *noisevarchrom, float madL[8][3],
                         float noisevar_ab, const bool useNoiseCCurve,
                         bool autoch, int denoiseNestedLevels)

{

    int maxlvl = WaveletCoeffs_L.maxlevel();
    int maxWL = 0, maxHL = 0;

    for (int lvl = 0; lvl < maxlvl; ++lvl) {
        if (WaveletCoeffs_L.level_W(lvl) > maxWL) {
            maxWL = WaveletCoeffs_L.level_W(lvl);
        }

        if (WaveletCoeffs_L.level_H(lvl) > maxHL) {
            maxHL = WaveletCoeffs_L.level_H(lvl);
        }
    }

#ifdef _OPENMP
#pragma omp parallel num_threads(                                              \
        denoiseNestedLevels) if (denoiseNestedLevels > 1)
#endif
    {
        float *buffer[3];
        buffer[0] = new /*(std::nothrow)*/ float[maxWL * maxHL + 32];
        buffer[1] = new /*(std::nothrow)*/ float[maxWL * maxHL + 64];
        buffer[2] = new /*(std::nothrow)*/ float[maxWL * maxHL + 96];

        // if (buffer[0] == nullptr || buffer[1] == nullptr || buffer[2] ==
        // nullptr) {

#ifdef _OPENMP
#pragma omp for schedule(dynamic) collapse(2)
#endif

        for (int lvl = 0; lvl < maxlvl; ++lvl) {
            for (int dir = 1; dir < 4; ++dir) {
                ShrinkAllAB(scale, WaveletCoeffs_L, WaveletCoeffs_ab,
                            buffer, lvl, dir, noisevarchrom, noisevar_ab,
                            useNoiseCCurve, autoch, madL[lvl]);
            }
        }

        for (int i = 2; i >= 0; i--) {
            if (buffer[i] != nullptr) {
                delete[] buffer[i];
            }
        }
    }
    return true;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void ShrinkAll_info(float **WavCoeffs_a, float **WavCoeffs_b, int W_ab,
                    int H_ab, float **noisevarlum, float **noisevarchrom,
                    float **noisevarhue, float &chaut, int &Nb, float &redaut,
                    float &blueaut, float &maxredaut, float &maxblueaut,
                    float &minredaut, float &minblueaut, int schoice, int lvl,
                    float &chromina, float &sigma, float &lumema,
                    float &sigma_L, float &redyel, float &skinc, float &nsknc,
                    float &maxchred, float &maxchblue, float &minchred,
                    float &minchblue, int &nb, float &chau, float &chred,
                    float &chblue)
{

    // simple wavelet shrinkage
    if (lvl == 1) { // only one time
        float chro = 0.f;
        float dev = 0.f;
        float devL = 0.f;
        int nc = 0;
        int nL = 0;
        int nry = 0;
        float lume = 0.f;
        float red_yel = 0.f;
        float skin_c = 0.f;
        int nsk = 0;

        for (int i = 0; i < H_ab; ++i) {
            for (int j = 0; j < W_ab; ++j) {
                chro += noisevarchrom[i][j];
                ++nc;
                dev += SQR(noisevarchrom[i][j] - (chro / nc));

                if (noisevarhue[i][j] > -0.8f && noisevarhue[i][j] < 2.0f &&
                    noisevarchrom[i][j] > 10000.f) { // saturated red yellow
                    red_yel += noisevarchrom[i][j];
                    ++nry;
                }

                if (noisevarhue[i][j] > 0.f && noisevarhue[i][j] < 1.6f &&
                    noisevarchrom[i][j] < 10000.f) { // skin
                    skin_c += noisevarchrom[i][j];
                    ++nsk;
                }

                lume += noisevarlum[i][j];
                ++nL;
                devL += SQR(noisevarlum[i][j] - (lume / nL));
            }
        }

        if (nc > 0) {
            chromina = chro / nc;
            sigma = sqrt(dev / nc);
            nsknc = static_cast<float>(nsk) / static_cast<float>(nc);
        } else {
            nsknc = static_cast<float>(nsk);
        }

        if (nL > 0) {
            lumema = lume / nL;
            sigma_L = sqrt(devL / nL);
        }

        if (nry > 0) {
            redyel = red_yel / nry;
        }

        if (nsk > 0) {
            skinc = skin_c / nsk;
        }
    }

    const float reduc =
        (schoice == 2) ? static_cast<float>(0.9 /*settings->nrhigh*/) : 1.f;

    for (int dir = 1; dir < 4; ++dir) {
        float mada, madb;
        mada = SQR(MadRgb(WavCoeffs_a[dir], W_ab * H_ab));

        chred += mada;

        if (mada > maxchred) {
            maxchred = mada;
        }

        if (mada < minchred) {
            minchred = mada;
        }

        maxredaut = sqrt(reduc * maxchred);
        minredaut = sqrt(reduc * minchred);

        madb = SQR(MadRgb(WavCoeffs_b[dir], W_ab * H_ab));
        chblue += madb;

        if (madb > maxchblue) {
            maxchblue = madb;
        }

        if (madb < minchblue) {
            minchblue = madb;
        }

        maxblueaut = sqrt(reduc * maxchblue);
        minblueaut = sqrt(reduc * minchblue);

        chau += (mada + madb);
        ++nb;
        // here evaluation of automatic
        chaut = sqrt(reduc * chau / (nb + nb));
        redaut = sqrt(reduc * chred / nb);
        blueaut = sqrt(reduc * chblue / nb);
        Nb = nb;
    }
}

void WaveletDenoiseAll_info(
    int levwav, wavelet_decomposition &WaveletCoeffs_a,
    wavelet_decomposition &WaveletCoeffs_b, float **noisevarlum,
    float **noisevarchrom, float **noisevarhue, float &chaut, int &Nb,
    float &redaut, float &blueaut, float &maxredaut, float &maxblueaut,
    float &minredaut, float &minblueaut, int schoice, float &chromina,
    float &sigma, float &lumema, float &sigma_L, float &redyel, float &skinc,
    float &nsknc, float &maxchred, float &maxchblue, float &minchred,
    float &minchblue, int &nb, float &chau, float &chred, float &chblue)
{

    int maxlvl = levwav;

    for (int lvl = 0; lvl < maxlvl; ++lvl) {

        int Wlvl_ab = WaveletCoeffs_a.level_W(lvl);
        int Hlvl_ab = WaveletCoeffs_a.level_H(lvl);

        float **WavCoeffs_a = WaveletCoeffs_a.level_coeffs(lvl);
        float **WavCoeffs_b = WaveletCoeffs_b.level_coeffs(lvl);

        ShrinkAll_info(WavCoeffs_a, WavCoeffs_b, Wlvl_ab, Hlvl_ab, noisevarlum,
                       noisevarchrom, noisevarhue, chaut, Nb, redaut, blueaut,
                       maxredaut, maxblueaut, minredaut, minblueaut, schoice,
                       lvl, chromina, sigma, lumema, sigma_L, redyel, skinc,
                       nsknc, maxchred, maxchblue, minchred, minchblue, nb,
                       chau, chred, chblue);
    }
}

/* How many wavelet levels to decompose to.  Stronger chroma sliders and the
 * aggressive (QUALITY_HIGH) mode ask for more levels; the image's short side
 * and the preview scale cap it.  8 is the hard ceiling (wavelet.h). */
int waveletLevels(float realred, float realblue, nrquality nrQuality,
                  double scale, int imwidth, int imheight)
{
    const float maxreal = max(realred, realblue);
    int levwav = maxreal < 8.f    ? 5
                 : maxreal < 10.f ? 6
                 : maxreal < 15.f ? 7
                                  : 8;

    if (nrQuality == QUALITY_HIGH) {
        levwav += 2; // settings->nrwavlevel; increase level for enhanced mode
    }

    levwav = min(8, levwav);
    levwav = max(5, int(levwav - std::ceil(std::log(scale))));

    const int minsizetile = min(imwidth, imheight);
    int maxlev2 = 8;

    if (minsizetile < 256) {
        maxlev2 = 7;
    }
    if (minsizetile < 128) {
        maxlev2 = 6;
    }
    if (minsizetile < 64) {
        maxlev2 = 5;
    }
    if (minsizetile < 32) {
        maxlev2 = 4;
    }
    if (minsizetile < 16) {
        maxlev2 = 3;
    }

    return min(maxlev2, levwav);
}

/* Phase 4, for one chroma channel.  In QUALITY_HIGH the bi-shrink pass runs
 * first and the plain pass runs over its output -- note madab is recomputed
 * inside ShrinkAllAB on the second pass, from the already-shrunk
 * coefficients, which is why the two passes are not idempotent. */
void shrinkChroma(const DenoiseContext &c, wavelet_decomposition &Ldecomp,
                  wavelet_decomposition &abdecomp, float madL[8][3],
                  float noisevar_ab, nrquality nrQuality, bool autoch)
{
    if (nrQuality == QUALITY_HIGH) {
        WaveletDenoiseAll_BiShrinkAB(c.scale, Ldecomp, abdecomp,
                                     c.noisevarchrom, madL, noisevar_ab,
                                     c.useNoiseCCurve, autoch,
                                     c.denoiseNestedLevels);
    }
    WaveletDenoiseAllAB(c.scale, Ldecomp, abdecomp, c.noisevarchrom, madL,
                        noisevar_ab, c.useNoiseCCurve, autoch,
                        c.denoiseNestedLevels);
}

/* Phases 2-6: decompose L/a/b, estimate the noise level, shrink, reconstruct.
 *
 * Ordering is forced by madL: it is computed from L's *original* detail
 * coefficients and is read by the a-, b- and L-shrinks alike, so L's bands
 * must not be modified until the chroma shrinks are done.  Hence
 * decompose(L) -> mad(L) -> {decompose,shrink,reconstruct}(a) ->
 * {...}(b) -> shrink(L) -> reconstruct(L).
 *
 * Lin is an out-parameter: phase 7 needs the *undenoised* L plane to form the
 * residual it runs the block DCT over, so it is snapshotted here, after the
 * luma shrink but before the reconstruction that overwrites labdn->L. */
void denoiseWavelet(const DenoiseContext &c, LabImage *labdn,
                    array2D<float> *&Lin, int levwav, nrquality nrQuality,
                    bool autoch, bool denoiseLuminance)
{
    const int threads = max(1, c.denoiseNestedLevels);

    wavelet_decomposition *Ldecomp;
    {
        Ldecomp = new wavelet_decomposition(labdn->L[0], labdn->W, labdn->H,
                                            levwav, 1, 1, threads);
    }

    /* madL[level][dir]: squared median absolute deviation of L's detail
     * coefficients, the robust noise estimate every shrink below scales by. */
    float madL[8][3];
    {
        const int maxlvl = Ldecomp->maxlevel();
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) collapse(2)                         \
    num_threads(c.denoiseNestedLevels) if (c.denoiseNestedLevels > 1)
#endif
        for (int lvl = 0; lvl < maxlvl; ++lvl) {
            for (int dir = 1; dir < 4; ++dir) {
                const int n = Ldecomp->level_W(lvl) * Ldecomp->level_H(lvl);
                float **WavCoeffs_L = Ldecomp->level_coeffs(lvl);
                madL[lvl][dir - 1] = SQR(MadRgb(WavCoeffs_L[dir], n));
            }
        }
    }

    for (int ch = 0; ch < 2; ++ch) {
        float *plane = ch == 0 ? labdn->a[0] : labdn->b[0];
        const float noisevar_ab = ch == 0 ? c.noisevarab_r : c.noisevarab_b;

        wavelet_decomposition *abdecomp;
        {
            abdecomp = new wavelet_decomposition(plane, labdn->W, labdn->H,
                                                 levwav, 1, 1, threads);
        }
        {
            shrinkChroma(c, *Ldecomp, *abdecomp, madL, noisevar_ab, nrQuality,
                         autoch);
        }
        {
            abdecomp->reconstruct(plane);
        }
        delete abdecomp;
    }

    if (denoiseLuminance) {
        {
            int edge = 0;
            if (nrQuality == QUALITY_HIGH) {
                WaveletDenoiseAll_BiShrinkL(c.scale, *Ldecomp, c.noisevarlum,
                                            madL, c.denoiseNestedLevels);
            }
            WaveletDenoiseAllL(c.scale, *Ldecomp, c.noisevarlum, madL, nullptr,
                               edge, c.denoiseNestedLevels);
        }
        {
            // snapshot L before reconstruction overwrites it -- phase 7 needs
            // the residual against the undenoised plane
            Lin = new array2D<float>(c.W, c.H);
#ifdef _OPENMP
#pragma omp parallel for num_threads(                                          \
        c.denoiseNestedLevels) if (c.denoiseNestedLevels > 1)
#endif
            for (int i = 0; i < c.H; ++i) {
                for (int j = 0; j < c.W; ++j) {
                    (*Lin)[i][j] = labdn->L[i][j];
                }
            }

            Ldecomp->reconstruct(labdn->L[0]);
        }
    }

    delete Ldecomp;
}

} // namespace denoise

namespace {

void adjust_params(procparams::DenoiseParams &dnparams, double scale)
{
    if (scale <= 1.0) {
        return;
    }

    const auto c = [](double x, double f) -> double {
        int s = SGN(x);
        double y = LIM01(std::abs(x) / 100.0);
        return s * intp(y, y * f, y) * 100.0;
    };

    double scale_factor = 1.0 / scale;
    double noise_factor_c = std::pow(scale_factor, 0.46);
    double noise_factor_l = std::pow(scale_factor, 0.62) * scale_factor;
    // noise_factor_l *= intp(std::pow(LIM01(dnparams.luminance / 100.0), 3.0),
    // scale_factor, 1.0); std::cout << "ADJUSTING LUMINANCE SCALE: " <<
    // noise_factor_l << std::endl; dnparams.luminance *= noise_factor_l;
    dnparams.luminance = c(dnparams.luminance, noise_factor_l);
    dnparams.luminanceDetail *= (1.0 + std::pow(1.0 - scale_factor, 2.2));
    dnparams.chrominance = c(dnparams.chrominance, noise_factor_c);
    dnparams.chrominanceRedGreen =
        c(dnparams.chrominanceRedGreen, noise_factor_c);
    dnparams.chrominanceBlueYellow =
        c(dnparams.chrominanceBlueYellow, noise_factor_c);
    // dnparams.chrominance *= noise_factor_c;
    // dnparams.chrominanceRedGreen *= noise_factor_c;
    // dnparams.chrominanceBlueYellow *= noise_factor_c;
}

void calcautodn_info(const ProcParams *params, float &chaut, float &delta,
                     int Nb, int levaut, float maxmax, float lumema,
                     float chromina, int mode, int lissage, float redyel,
                     float skinc, float nsknc)
{

    float reducdelta = 1.f;

    if (params->denoise.aggressive) {
        reducdelta = static_cast<float>(0.9 /*settings->nrhigh*/);
    }

    chaut =
        (chaut * Nb - maxmax) / (Nb - 1); // suppress maximum for chaut calcul

    if ((redyel > 5000.f || skinc > 1000.f) && nsknc < 0.4f &&
        chromina > 3000.f) {
        chaut *= 0.45f; // reduct action in red zone, except skin for high / med
                        // chroma
    } else if ((redyel > 12000.f || skinc > 1200.f) && nsknc < 0.3f &&
               chromina > 3000.f) {
        chaut *= 0.3f;
    }

    if (mode == 0 || mode == 2) { // Preview or Auto multizone
        if (chromina > 10000.f) {
            chaut *= 0.7f; // decrease action for high chroma  (visible noise)
        } else if (chromina > 6000.f) {
            chaut *= 0.9f;
        } else if (chromina < 3000.f) {
            chaut *= 1.2f; // increase action in low chroma==> 1.2  /==>2.0 ==>
                           // curve CC
        } else if (chromina < 2000.f) {
            chaut *= 1.5f; // increase action in low chroma==> 1.5 / ==>2.7
        }

        if (lumema < 2500.f) {
            chaut *= 1.3f; // increase action for low light
        } else if (lumema < 5000.f) {
            chaut *= 1.2f;
        } else if (lumema > 20000.f) {
            chaut *= 0.9f; // decrease for high light
        }
    } else if (mode == 1) { // auto ==> less coefficient because interaction
        if (chromina > 10000.f) {
            chaut *= 0.8f; // decrease action for high chroma  (visible noise)
        } else if (chromina > 6000.f) {
            chaut *= 0.9f;
        } else if (chromina < 3000.f) {
            chaut *= 1.5f; // increase action in low chroma
        } else if (chromina < 2000.f) {
            chaut *= 2.2f; // increase action in low chroma
        }

        if (lumema < 2500.f) {
            chaut *= 1.2f; // increase action for low light
        } else if (lumema < 5000.f) {
            chaut *= 1.1f;
        } else if (lumema > 20000.f) {
            chaut *= 0.9f; // decrease for high light
        }
    }

    if (levaut == 0) { // Low denoise
        if (chaut > 300.f) {
            chaut = 0.714286f * chaut + 85.71428f;
        }
    }

    delta = maxmax - chaut;
    delta *= reducdelta;

    if (lissage == 1 || lissage == 2) {
        if (chaut < 200.f && delta < 200.f) {
            delta *= 0.95f;
        } else if (chaut < 200.f && delta < 400.f) {
            delta *= 0.5f;
        } else if (chaut < 200.f && delta >= 400.f) {
            delta = 200.f;
        } else if (chaut < 400.f && delta < 400.f) {
            delta *= 0.4f;
        } else if (chaut < 400.f && delta >= 400.f) {
            delta = 120.f;
        } else if (chaut < 550.f) {
            delta *= 0.15f;
        } else if (chaut < 650.f) {
            delta *= 0.1f;
        } else { /*if (chaut >= 650.f)*/
            delta *= 0.07f;
        }

        if (mode == 0 || mode == 2) { // Preview or Auto multizone
            if (chromina < 6000.f) {
                delta *= 1.4f; // increase maxi
            }

            if (lumema < 5000.f) {
                delta *= 1.4f;
            }
        } else if (mode == 1) { // Auto
            if (chromina < 6000.f) {
                delta *= 1.2f; // increase maxi
            }

            if (lumema < 5000.f) {
                delta *= 1.2f;
            }
        }
    }

    if (lissage == 0) {
        if (chaut < 200.f && delta < 200.f) {
            delta *= 0.95f;
        } else if (chaut < 200.f && delta < 400.f) {
            delta *= 0.7f;
        } else if (chaut < 200.f && delta >= 400.f) {
            delta = 280.f;
        } else if (chaut < 400.f && delta < 400.f) {
            delta *= 0.6f;
        } else if (chaut < 400.f && delta >= 400.f) {
            delta = 200.f;
        } else if (chaut < 550.f) {
            delta *= 0.3f;
        } else if (chaut < 650.f) {
            delta *= 0.2f;
        } else { /*if (chaut >= 650.f)*/
            delta *= 0.15f;
        }

        if (mode == 0 || mode == 2) { // Preview or Auto multizone
            if (chromina < 6000.f) {
                delta *= 1.4f; // increase maxi
            }

            if (lumema < 5000.f) {
                delta *= 1.4f;
            }
        } else if (mode == 1) { // Auto
            if (chromina < 6000.f) {
                delta *= 1.2f; // increase maxi
            }

            if (lumema < 5000.f) {
                delta *= 1.2f;
            }
        }
    }
}

void RGB_denoise_infoGamCurve(const procparams::DenoiseParams &dnparams,
                              bool isRAW, LUTf &gamcurve, float &gam,
                              float &gamthresh, float &gamslope)
{
    gam = dnparams.gamma;
    gamthresh = 0.001f;

    if (!isRAW) { // reduce gamma under 1 for Lab mode ==> TIF and JPG
        if (gam < 1.9f) {
            gam = 1.f - (1.9f - gam) / 3.f; // minimum gamma 0.7
        } else if (gam >= 1.9f && gam <= 3.f) {
            gam = (1.4f / 1.1f) * gam - 1.41818f;
        }
    }

    gamslope = exp(log(static_cast<double>(gamthresh)) / gam) / gamthresh;
    Color::gammaf2lut(gamcurve, gam, gamthresh, gamslope, 65535.f, 32768.f);
}

void RGB_denoise_info(ImProcData &im, Imagefloat *src, Imagefloat *provicalc,
                      const bool isRAW, LUTf &gamcurve, float gam,
                      float gamthresh, float gamslope,
                      const procparams::DenoiseParams &dnparams,
                      const double expcomp, float &chaut, int &Nb,
                      float &redaut, float &blueaut, float &maxredaut,
                      float &maxblueaut, float &minredaut, float &minblueaut,
                      float &chromina, float &sigma, float &lumema,
                      float &sigma_L, float &redyel, float &skinc, float &nsknc)
{
    const ProcParams *params = im.params;
    double scale = im.scale;
    bool multiThread = im.multiThread;

    if (dnparams.chrominanceMethod !=
        procparams::DenoiseParams::ChrominanceMethod::AUTOMATIC) {
        // nothing to do
        return;
    }

    int hei, wid;
    float **lumcalc;
    float **acalc;
    float **bcalc;
    hei = provicalc->getHeight();
    wid = provicalc->getWidth();
    TMatrix wprofi =
        ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);

    const float wpi[3][3] = {
        {static_cast<float>(wprofi[0][0]), static_cast<float>(wprofi[0][1]),
         static_cast<float>(wprofi[0][2])},
        {static_cast<float>(wprofi[1][0]), static_cast<float>(wprofi[1][1]),
         static_cast<float>(wprofi[1][2])},
        {static_cast<float>(wprofi[2][0]), static_cast<float>(wprofi[2][1]),
         static_cast<float>(wprofi[2][2])}};

    lumcalc = new float *[hei];

    for (int i = 0; i < hei; ++i) {
        lumcalc[i] = new float[wid];
    }

    acalc = new float *[hei];

    for (int i = 0; i < hei; ++i) {
        acalc[i] = new float[wid];
    }

    bcalc = new float *[hei];

    for (int i = 0; i < hei; ++i) {
        bcalc[i] = new float[wid];
    }

#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif

    for (int ii = 0; ii < hei; ++ii) {
        for (int jj = 0; jj < wid; ++jj) {
            float LLum, AAum, BBum;
            float RL = provicalc->r(ii, jj);
            float GL = provicalc->g(ii, jj);
            float BL = provicalc->b(ii, jj);
            // determine luminance for noisecurve
            float XL, YL, ZL;
            Color::rgbxyz(RL, GL, BL, XL, YL, ZL, wpi);
            Color::XYZ2Lab(XL, YL, ZL, LLum, AAum, BBum);
            lumcalc[ii][jj] = LLum;
            acalc[ii][jj] = AAum;
            bcalc[ii][jj] = BBum;
        }
    }

    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

    const int imheight = src->getHeight(), imwidth = src->getWidth();
    const float gain = pow(2.0f, float(expcomp));

    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

    TMatrix wprof =
        ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    const float wp[3][3] = {
        {static_cast<float>(wprof[0][0]), static_cast<float>(wprof[0][1]),
         static_cast<float>(wprof[0][2])},
        {static_cast<float>(wprof[1][0]), static_cast<float>(wprof[1][1]),
         static_cast<float>(wprof[1][2])},
        {static_cast<float>(wprof[2][0]), static_cast<float>(wprof[2][1]),
         static_cast<float>(wprof[2][2])}};

    float chau = 0.f;
    float chred = 0.f;
    float chblue = 0.f;
    float maxchred = 0.f;
    float maxchblue = 0.f;
    float minchred = 100000000.f;
    float minchblue = 100000000.f;
    int nb = 0;
    int comptlevel = 0;

    {
        {
            const int tiletop = 0, tileleft = 0;
            const int tileright = imwidth, tilebottom = imheight;
            const int width = imwidth, height = imheight;
            LabImage *labdn = new LabImage(width, height);
            float **noisevarlum = new float *[(height + 1) / 2];

            for (int i = 0; i < (height + 1) / 2; ++i) {
                noisevarlum[i] = new float[(width + 1) / 2];
            }

            float **noisevarchrom = new float *[(height + 1) / 2];

            for (int i = 0; i < (height + 1) / 2; ++i) {
                noisevarchrom[i] = new float[(width + 1) / 2];
            }

            float **noisevarhue = new float *[(height + 1) / 2];

            for (int i = 0; i < (height + 1) / 2; ++i) {
                noisevarhue[i] = new float[(width + 1) / 2];
            }

            float realred, realblue;
            float interm_med =
                1.5f; // static_cast<float>(dnparams.chrominance) / 10.0;
            float intermred, intermblue;

            intermred = 0.f;
            intermblue = 0.f;
            // if (dnparams.chrominanceRedGreen > 0.) {
            //     intermred = (dnparams.chrominanceRedGreen / 10.);
            // } else {
            //     intermred = static_cast<float>(dnparams.chrominanceRedGreen)
            //     / 7.0;     //increase slower than linear for more sensit
            // }

            // if (dnparams.chrominanceBlueYellow > 0.) {
            //     intermblue = (dnparams.chrominanceBlueYellow / 10.);
            // } else {
            //     intermblue =
            //     static_cast<float>(dnparams.chrominanceBlueYellow) / 7.0;
            //     //increase slower than linear for more sensit
            // }

            realred = interm_med + intermred;

            if (realred < 0.f) {
                realred = 0.001f;
            }

            realblue = interm_med + intermblue;

            if (realblue < 0.f) {
                realblue = 0.001f;
            }

            // fill tile from image; convert RGB to "luma/chroma"

            if (isRAW) { // image is raw; use channel differences for chroma
                         // channels
#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif

                for (int i = tiletop; i < tilebottom; i += 2) {
                    int i1 = i - tiletop;
#ifdef ART_SIMD
                    __m128 aNv, bNv;
                    __m128 c100v = _mm_set1_ps(100.f);
                    int j;

                    for (j = tileleft; j < tileright - 7; j += 8) {
                        int j1 = j - tileleft;
                        aNv = LVFU(acalc[i >> 1][j >> 1]);
                        bNv = LVFU(bcalc[i >> 1][j >> 1]);
                        _mm_storeu_ps(&noisevarhue[i1 >> 1][j1 >> 1],
                                      xatan2f(bNv, aNv));
                        _mm_storeu_ps(
                            &noisevarchrom[i1 >> 1][j1 >> 1],
                            vmaxf(vsqrtf(SQRV(aNv) + SQRV(bNv)), c100v));
                    }

                    for (; j < tileright; j += 2) {
                        int j1 = j - tileleft;
                        float aN = acalc[i >> 1][j >> 1];
                        float bN = bcalc[i >> 1][j >> 1];
                        float cN = sqrtf(SQR(aN) + SQR(bN));
                        noisevarhue[i1 >> 1][j1 >> 1] = xatan2f(bN, aN);

                        if (cN < 100.f) {
                            cN = 100.f; // avoid divided by zero
                        }

                        noisevarchrom[i1 >> 1][j1 >> 1] = cN;
                    }

#else

                    for (int j = tileleft; j < tileright; j += 2) {
                        int j1 = j - tileleft;
                        float aN = acalc[i >> 1][j >> 1];
                        float bN = bcalc[i >> 1][j >> 1];
                        float cN = sqrtf(SQR(aN) + SQR(bN));
                        float hN = xatan2f(bN, aN);

                        if (cN < 100.f) {
                            cN = 100.f; // avoid divided by zero
                        }

                        noisevarchrom[i1 >> 1][j1 >> 1] = cN;
                        noisevarhue[i1 >> 1][j1 >> 1] = hN;
                    }

#endif
                }

#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif

                for (int i = tiletop; i < tilebottom; i += 2) {
                    int i1 = i - tiletop;

                    for (int j = tileleft; j < tileright; j += 2) {
                        int j1 = j - tileleft;
                        float Llum = lumcalc[i >> 1][j >> 1];
                        Llum =
                            Llum < 2.f ? 2.f : Llum; // avoid divided by zero ?
                        Llum = Llum > 32768.f ? 32768.f
                                              : Llum; // not strictly necessary
                        noisevarlum[i1 >> 1][j1 >> 1] = Llum;
                    }
                }

                for (int i = tiletop /*, i1=0*/; i < tilebottom;
                     ++i /*, ++i1*/) {
                    int i1 = i - tiletop;

                    for (int j = tileleft /*, j1=0*/; j < tileright;
                         ++j /*, ++j1*/) {
                        int j1 = j - tileleft;

                        float X = gain * src->r(i, j);
                        float Y = gain * src->g(i, j);
                        float Z = gain * src->b(i, j);

                        X = X < 65535.f ? gamcurve[X]
                                        : (Color::gammaf(X / 65535.f, gam,
                                                         gamthresh, gamslope) *
                                           32768.f);
                        Y = Y < 65535.f ? gamcurve[Y]
                                        : (Color::gammaf(Y / 65535.f, gam,
                                                         gamthresh, gamslope) *
                                           32768.f);
                        Z = Z < 65535.f ? gamcurve[Z]
                                        : (Color::gammaf(Z / 65535.f, gam,
                                                         gamthresh, gamslope) *
                                           32768.f);

                        // labdn->a[i1][j1] = (X - Y);
                        // labdn->b[i1][j1] = (Y - Z);
                        float l, u, v;
                        Color::rgb2yuv(X, Y, Z, l, u, v, wp);
                        labdn->a[i1][j1] = v;
                        labdn->b[i1][j1] = u;
                    }
                }
            } else { // image is not raw; use Lab parametrization
                for (int i = tiletop /*, i1=0*/; i < tilebottom;
                     ++i /*, ++i1*/) {
                    int i1 = i - tiletop;

                    for (int j = tileleft /*, j1=0*/; j < tileright;
                         ++j /*, ++j1*/) {
                        int j1 = j - tileleft;
                        // float L, a, b;
                        float rLum = src->r(i, j); // for luminance denoise
                                                   // curve
                        float gLum = src->g(i, j);
                        float bLum = src->b(i, j);

                        // use gamma sRGB, not good if TIF (JPG) Output profil
                        // not with gamma sRGB  (eg : gamma =1.0, or 1.8...)
                        // very difficult to solve !
                        //  solution ==> save TIF with gamma sRGB and re open
                        float rtmp = Color::igammatab_srgb[src->r(i, j)];
                        float gtmp = Color::igammatab_srgb[src->g(i, j)];
                        float btmp = Color::igammatab_srgb[src->b(i, j)];
                        // modification Jacques feb 2013
                        //  gamma slider different from raw
                        rtmp = rtmp < 65535.f
                                   ? gamcurve[rtmp]
                                   : (Color::gammanf(rtmp / 65535.f, gam) *
                                      32768.f);
                        gtmp = gtmp < 65535.f
                                   ? gamcurve[gtmp]
                                   : (Color::gammanf(gtmp / 65535.f, gam) *
                                      32768.f);
                        btmp = btmp < 65535.f
                                   ? gamcurve[btmp]
                                   : (Color::gammanf(btmp / 65535.f, gam) *
                                      32768.f);

                        // float X, Y, Z;
                        // Color::rgbxyz(rtmp, gtmp, btmp, X, Y, Z, wp);

                        // //convert Lab
                        // Color::XYZ2Lab(X, Y, Z, L, a, b);
                        float Y, u, v;
                        Color::rgb2yuv(rtmp, gtmp, btmp, Y, u, v, wp);

                        if (((i1 | j1) & 1) == 0) {
                            float Llum, alum, blum;
                            float XL, YL, ZL;
                            Color::rgbxyz(rLum, gLum, bLum, XL, YL, ZL, wp);
                            Color::XYZ2Lab(XL, YL, ZL, Llum, alum, blum);
                            float kN = Llum;

                            if (kN < 2.f) {
                                kN = 2.f;
                            }

                            if (kN > 32768.f) {
                                kN = 32768.f;
                            }

                            noisevarlum[i1 >> 1][j1 >> 1] = kN;
                            float aN = alum;
                            float bN = blum;
                            float hN = xatan2f(bN, aN);
                            float cN = sqrt(SQR(aN) + SQR(bN));

                            if (cN < 100.f) {
                                cN = 100.f; // avoid divided by zero
                            }

                            noisevarchrom[i1 >> 1][j1 >> 1] = cN;
                            noisevarhue[i1 >> 1][j1 >> 1] = hN;
                        }

                        labdn->a[i1][j1] = v;
                        labdn->b[i1][j1] = u;
                    }
                }
            }

            int datalen = labdn->W * labdn->H;

            // now perform basic wavelet denoise
            // last two arguments of wavelet decomposition are max number of
            // wavelet decomposition levels; and whether to subsample the image
            // after wavelet filtering.  Subsampling is coded as binary 1 or 0
            // for each level, eg subsampling = 0 means no subsampling, 1 means
            // subsample the first level only, 7 means subsample the first three
            // levels, etc.

            wavelet_decomposition *adecomp;
            wavelet_decomposition *bdecomp;

            int schoice = 0; // shrink method

            if (dnparams.aggressive) {
                schoice = 2;
            }

            const int levwav = max(2, int(5 - std::ceil(std::log(scale))));
#ifdef _OPENMP
#pragma omp parallel sections if (multiThread)
#endif
            {
#ifdef _OPENMP
#pragma omp section
#endif
                {
                    adecomp = new wavelet_decomposition(
                        labdn->data + datalen, labdn->W, labdn->H, levwav, 1);
                }
#ifdef _OPENMP
#pragma omp section
#endif
                {
                    bdecomp = new wavelet_decomposition(
                        labdn->data + 2 * datalen, labdn->W, labdn->H, levwav,
                        1);
                }
            }

            if (comptlevel == 0) {
                denoise::WaveletDenoiseAll_info(
                    levwav, *adecomp, *bdecomp, noisevarlum, noisevarchrom,
                    noisevarhue, chaut, Nb, redaut, blueaut, maxredaut,
                    maxblueaut, minredaut, minblueaut, schoice, chromina, sigma,
                    lumema, sigma_L, redyel, skinc, nsknc, maxchred, maxchblue,
                    minchred, minchblue, nb, chau, chred,
                    chblue); // Enhance mode
            }

            comptlevel += 1;
            delete adecomp;
            delete bdecomp;
            delete labdn;

            for (int i = 0; i < (height + 1) / 2; ++i) {
                delete[] noisevarlum[i];
            }

            delete[] noisevarlum;

            for (int i = 0; i < (height + 1) / 2; ++i) {
                delete[] noisevarchrom[i];
            }

            delete[] noisevarchrom;

            for (int i = 0; i < (height + 1) / 2; ++i) {
                delete[] noisevarhue[i];
            }

            delete[] noisevarhue;

        } // end of tile row
    } // end of tile loop

    for (int i = 0; i < hei; ++i) {
        delete[] lumcalc[i];
    }

    delete[] lumcalc;

    for (int i = 0; i < hei; ++i) {
        delete[] acalc[i];
    }

    delete[] acalc;

    for (int i = 0; i < hei; ++i) {
        delete[] bcalc[i];
    }

    delete[] bcalc;

#undef TS
// #undef fTS
#undef offset
#undef epsilon

} // End of main RGB_denoise

} // namespace

namespace denoise {

NoiseCurve::NoiseCurve(): sum(0.f) {}

void NoiseCurve::Reset()
{
    lutNoiseCurve.reset();
    sum = 0.f;
}

void NoiseCurve::Set(const Curve &pCurve)
{
    if (pCurve.isIdentity()) {
        Reset(); // raise this value if the quality suffers from this number of
                 // samples
        return;
    }

    lutNoiseCurve(501); // raise this value if the quality suffers from this
                        // number of samples
    sum = 0.f;

    for (int i = 0; i < 501; i++) {
        lutNoiseCurve[i] = pCurve.getVal(double(i) / 500.);

        if (lutNoiseCurve[i] < 0.01f) {
            lutNoiseCurve[i] = 0.01f; // avoid 0.f for wavelet : under 0.01f
                                      // quasi no action for each value
        }

        sum += lutNoiseCurve[i]; // minima for Wavelet about 6.f or 7.f quasi no
                                 // action
    }

    // lutNoisCurve.dump("Nois");
}

void NoiseCurve::Set(const std::vector<double> &curvePoints)
{

    if (!curvePoints.empty() && curvePoints[0] > FCT_Linear &&
        curvePoints[0] < FCT_Unchanged) {
        FlatCurve tcurve(curvePoints, false, CURVES_MIN_POLY_POINTS / 2);
        tcurve.setIdentityValue(0.);
        Set(tcurve);
    } else {
        Reset();
    }
}

} // namespace denoise

void ImProcFunctions::DenoiseInfoStore::reset()
{
    chM = 0;
    for (int i = 0; i < 9; ++i) {
        max_r[i] = 0.f;
        max_b[i] = 0.f;
        ch_M[i] = 0.f;
    }
    valid = false;
    DenoiseParams p;
    chrominance = p.chrominance;
    chrominanceRedGreen = p.chrominanceRedGreen;
    chrominanceBlueYellow = p.chrominanceBlueYellow;
}

bool ImProcFunctions::DenoiseInfoStore::update_pparams(
    const procparams::ProcParams &p)
{
    if (!valid) {
        // std::cout << "** INVALID ** " << std::endl;
        pparams = p;
        return false;
    } else {
        const auto &d1 = pparams.denoise;
        const auto &d2 = p.denoise;
        const auto dn_eq = [&]() -> bool {
            return (d1.enabled == d2.enabled) &&
                   (d1.colorSpace == d2.colorSpace) &&
                   (d1.aggressive == d2.aggressive) && (d1.gamma == d2.gamma);
        };
        const auto &w1 = pparams.wb;
        const auto &w2 = p.wb;
        const auto wb_eq = [&]() -> bool {
            if (w1.enabled == w2.enabled && w1.method == w2.method &&
                (w1.method == procparams::WBParams::CAMERA ||
                 w1.method == procparams::WBParams::AUTO)) {
                return true;
            }
            return w1 == w2;
        };
        const auto &e1 = pparams.exposure;
        const auto &e2 = p.exposure;
        const auto exposure_eq = [&]() -> bool {
            return e1.enabled == e2.enabled && e1.hrmode == e2.hrmode;
        };
        const auto &r1 = pparams.raw;
        const auto &r2 = p.raw;
        const auto raw_eq = [&]() -> bool {
            auto r2b = r2;
#define MK_EQ_(k) r2b.k = r1.k
            MK_EQ_(bayersensor.method);
            MK_EQ_(bayersensor.lmmse_iterations);
            MK_EQ_(bayersensor.dualDemosaicAutoContrast);
            MK_EQ_(bayersensor.dualDemosaicContrast);
            MK_EQ_(xtranssensor.method);
#undef MK_EQ_
            return r1 == r2b;
        };
        const bool changed =
            !dn_eq() || !wb_eq() || !exposure_eq() || !raw_eq();
        // if (changed) {
        //     std::cout << "** CHANGED " << std::endl;
        // }
        pparams = p;
        return !changed;
    }
}

void ImProcFunctions::denoiseComputeParams(ImageSource *imgsrc,
                                           const ColorTemp &currWB,
                                           DenoiseInfoStore &store,
                                           procparams::DenoiseParams &dnparams)
{
    if (store.valid ||
        dnparams.chrominanceMethod !=
            procparams::DenoiseParams::ChrominanceMethod::AUTOMATIC) {
        if (dnparams.chrominanceMethod ==
            procparams::DenoiseParams::ChrominanceMethod::AUTOMATIC) {
            dnparams.chrominance =
                store.chrominance * dnparams.chrominanceAutoFactor;
            dnparams.chrominanceRedGreen =
                store.chrominanceRedGreen * dnparams.chrominanceAutoFactor;
            dnparams.chrominanceBlueYellow =
                store.chrominanceBlueYellow * dnparams.chrominanceAutoFactor;
        }
        return;
    }

    if (settings->verbose) {
        std::cout << "Denoise: computing auto chrominance params..."
                  << std::endl;
    }

    float autoNR = 10;    // settings->nrauto;
    float autoNRmax = 40; // settings->nrautomax;

    int widIm, heiIm;
    int tr = getCoarseBitMask(params->coarse);
    imgsrc->getFullSize(widIm, heiIm, tr);

    float min_b[9];
    float min_r[9];
    float lumL[9];
    float chromC[9];
    float ry[9];
    float sk[9];
    float pcsk[9];

    if (!store.valid &&
        dnparams.chrominanceMethod ==
            procparams::DenoiseParams::ChrominanceMethod::AUTOMATIC) {
        MyTime t1aue, t2aue;
        t1aue.set();

        store.reset(); // = DenoiseInfoStore();

        int crW = 100; // settings->leveldnv == 0
        int crH = 100; // settings->leveldnv == 0

        // if (settings->leveldnv == 1) {
        //     crW = 250;
        //     crH = 250;
        // }

        // if (settings->leveldnv == 2) {
        crW = widIm / 2;
        crH = heiIm / 2;
        // }

        // if (settings->leveldnv == 3) {
        //     crW = tileWskip - 10;
        //     crH = tileHskip - 10;
        // }

        float lowdenoise = 1.f;
        int levaut = 0;

        // if (levaut == 1) { //Standard
        //     lowdenoise = 0.7f;
        // }

        LUTf gamcurve(65536, 0);
        float gam, gamthresh, gamslope;
        RGB_denoise_infoGamCurve(dnparams, imgsrc->isRAW(), gamcurve, gam,
                                 gamthresh, gamslope);
        int Nb[9];

#ifdef _OPENMP
#pragma omp parallel if (multiThread)
#endif
        {
            Imagefloat *origCropPart =
                new Imagefloat(crW, crH); // allocate memory
            Imagefloat *provicalc = new Imagefloat(
                (crW + 1) / 2, (crH + 1) / 2); // for denoise curves

            int coordW[3]; // coordinate of part of image to measure noise
            int coordH[3];
            int begW = 50;
            int begH = 50;
            coordW[0] = begW;
            coordW[1] = widIm / 2 - crW / 2;
            coordW[2] = widIm - crW - begW;
            coordH[0] = begH;
            coordH[1] = heiIm / 2 - crH / 2;
            coordH[2] = heiIm - crH - begH;

#ifdef _OPENMP
#pragma omp for schedule(dynamic) collapse(2) nowait
#endif

            for (int wcr = 0; wcr <= 2; wcr++) {
                for (int hcr = 0; hcr <= 2; hcr++) {
                    PreviewProps ppP(coordW[wcr], coordH[hcr], crW, crH, 1);
                    imgsrc->getImage(currWB, tr, origCropPart, ppP,
                                     params->exposure, params->raw);

                    // we only need image reduced to 1/4 here
                    for (int ii = 0; ii < crH; ii += 2) {
                        for (int jj = 0; jj < crW; jj += 2) {
                            provicalc->r(ii >> 1, jj >> 1) =
                                origCropPart->r(ii, jj);
                            provicalc->g(ii >> 1, jj >> 1) =
                                origCropPart->g(ii, jj);
                            provicalc->b(ii >> 1, jj >> 1) =
                                origCropPart->b(ii, jj);
                        }
                    }

                    imgsrc->convertColorSpace(
                        provicalc, params->icm,
                        currWB); // for denoise luminance curve

                    float pondcorrec = 1.0f;
                    float chaut = 0.f, redaut = 0.f, blueaut = 0.f,
                          maxredaut = 0.f, maxblueaut = 0.f, minredaut = 0.f,
                          minblueaut = 0.f, chromina = 0.f, sigma = 0.f,
                          lumema = 0.f, sigma_L = 0.f, redyel = 0.f,
                          skinc = 0.f, nsknc = 0.f;
                    int nb = 0;
                    ImProcData im(params, 1.f /*scale*/, multiThread);
                    RGB_denoise_info(
                        im, origCropPart, provicalc, imgsrc->isRAW(), gamcurve,
                        gam, gamthresh, gamslope, dnparams,
                        std::log(5.f) /
                            std::log(2.f) /*imgsrc->getDirPyrDenoiseExpComp()*/,
                        chaut, nb, redaut, blueaut, maxredaut, maxblueaut,
                        minredaut, minblueaut, chromina, sigma, lumema, sigma_L,
                        redyel, skinc, nsknc);

                    // printf("DCROP skip=%d cha=%f red=%f bl=%f redM=%f bluM=%f
                    // chrom=%f sigm=%f lum=%f\n",skip, chaut,redaut,blueaut,
                    // maxredaut, maxblueaut, chromina, sigma, lumema);
                    Nb[hcr * 3 + wcr] = nb;
                    store.ch_M[hcr * 3 + wcr] = pondcorrec * chaut;
                    store.max_r[hcr * 3 + wcr] = pondcorrec * maxredaut;
                    store.max_b[hcr * 3 + wcr] = pondcorrec * maxblueaut;
                    min_r[hcr * 3 + wcr] = pondcorrec * minredaut;
                    min_b[hcr * 3 + wcr] = pondcorrec * minblueaut;
                    lumL[hcr * 3 + wcr] = lumema;
                    chromC[hcr * 3 + wcr] = chromina;
                    ry[hcr * 3 + wcr] = redyel;
                    sk[hcr * 3 + wcr] = skinc;
                    pcsk[hcr * 3 + wcr] = nsknc;
                }
            }

            delete provicalc;
            delete origCropPart;
        }

        float chM = 0.f;
        float MaxR = 0.f;
        float MaxB = 0.f;
        float MinR = 100000000000.f;
        float MinB = 100000000000.f;
        float maxr = 0.f;
        float maxb = 0.f;
        float Max_R[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        float Max_B[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        float Min_R[9];
        float Min_B[9];
        float MaxRMoy = 0.f;
        float MaxBMoy = 0.f;
        float MinRMoy = 0.f;
        float MinBMoy = 0.f;

        float multip = 1.f;

        if (!imgsrc->isRAW()) {
            multip = 2.f; // take into account gamma for TIF / JPG approximate
                          // value...not good for gamma=1
        }

        float adjustr = 1.f;

        // if (params->icm.workingProfile == "ProPhoto")   {
        //     adjustr = 1.f;   //
        // } else if (params->icm.workingProfile == "Adobe RGB")  {
        //     adjustr = 1.f / 1.3f;
        // } else if (params->icm.workingProfile == "sRGB")       {
        //     adjustr = 1.f / 1.3f;
        // } else if (params->icm.workingProfile == "WideGamut")  {
        //     adjustr = 1.f / 1.1f;
        // } else if (params->icm.workingProfile == "Beta RGB")   {
        //     adjustr = 1.f / 1.2f;
        // } else if (params->icm.workingProfile == "BestRGB")    {
        //     adjustr = 1.f / 1.2f;
        // } else if (params->icm.workingProfile == "BruceRGB")   {
        //     adjustr = 1.f / 1.2f;
        // }

        float delta[9];
        int mode = 1;
        int lissage = 0; // settings->leveldnliss;

        for (int k = 0; k < 9; k++) {
            float maxmax = max(store.max_r[k], store.max_b[k]);
            calcautodn_info(params, store.ch_M[k], delta[k], Nb[k], levaut,
                            maxmax, lumL[k], chromC[k], mode, lissage, ry[k],
                            sk[k], pcsk[k]);
            //  printf("ch_M=%f delta=%f\n",ch_M[k], delta[k]);
        }

        for (int k = 0; k < 9; k++) {
            if (store.max_r[k] > store.max_b[k]) {
                Max_R[k] = (delta[k]) /
                           ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                Min_B[k] = -(store.ch_M[k] - min_b[k]) /
                           (autoNRmax * multip * adjustr * lowdenoise);
                Max_B[k] = 0.f;
                Min_R[k] = 0.f;
            } else {
                Max_B[k] = (delta[k]) /
                           ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                Min_R[k] = -(store.ch_M[k] - min_r[k]) /
                           (autoNRmax * multip * adjustr * lowdenoise);
                Min_B[k] = 0.f;
                Max_R[k] = 0.f;
            }
        }

        for (int k = 0; k < 9; k++) {
            //  printf("ch_M= %f Max_R=%f Max_B=%f min_r=%f
            //  min_b=%f\n",ch_M[k],Max_R[k], Max_B[k],Min_R[k], Min_B[k]);
            chM += store.ch_M[k];
            MaxBMoy += Max_B[k];
            MaxRMoy += Max_R[k];
            MinRMoy += Min_R[k];
            MinBMoy += Min_B[k];

            if (Max_R[k] > MaxR) {
                MaxR = Max_R[k];
            }

            if (Max_B[k] > MaxB) {
                MaxB = Max_B[k];
            }

            if (Min_R[k] < MinR) {
                MinR = Min_R[k];
            }

            if (Min_B[k] < MinB) {
                MinB = Min_B[k];
            }
        }

        chM /= 9;
        MaxBMoy /= 9;
        MaxRMoy /= 9;
        MinBMoy /= 9;
        MinRMoy /= 9;

        if (MaxR > MaxB) {
            maxr = MaxRMoy + (MaxR - MaxRMoy) * 0.66f; // #std Dev
            // maxb=MinB;
            maxb = MinBMoy + (MinB - MinBMoy) * 0.66f;
        } else {
            maxb = MaxBMoy + (MaxB - MaxBMoy) * 0.66f;
            maxr = MinRMoy + (MinR - MinRMoy) * 0.66f;
        }

        //                  printf("DCROP skip=%d cha=%f red=%f bl=%f \n",skip,
        //                  chM,maxr,maxb);
        store.chrominance = chM / (autoNR * multip * adjustr);
        store.chrominanceRedGreen = maxr;
        store.chrominanceBlueYellow = maxb;

        dnparams.chrominance =
            store.chrominance * dnparams.chrominanceAutoFactor;
        dnparams.chrominanceRedGreen =
            store.chrominanceRedGreen * dnparams.chrominanceAutoFactor;
        dnparams.chrominanceBlueYellow =
            store.chrominanceBlueYellow * dnparams.chrominanceAutoFactor;

        store.valid = true;

        // printf("DENOISE STORE FINAL:\n  chM = %.6f", store.chM);
        // printf("  max_r = {");
        // for (int i = 0; i < 9; ++i) printf(" %.6f", store.max_r[i]);
        // printf(" }\n  max_b = {");
        // for (int i = 0; i < 9; ++i) printf(" %.6f", store.max_b[i]);
        // printf(" }\n  ch_M = {");
        // for (int i = 0; i < 9; ++i) printf(" %.6f", store.ch_M[i]);
        // printf("}\n");
        // printf("*****************\n\n");
        // fflush(stdout);

        if (settings->verbose) {
            t2aue.set();
            printf("Info denoise auto performed in %d usec:\n",
                   t2aue.etime(t1aue));
        }
        // end evaluate noise
    }
}
void ImProcFunctions::denoise(ImageSource *imgsrc, const ColorTemp &currWB,
                              Imagefloat *img,
                              const procparams::DenoiseParams &dnparams)
{
    if (!dnparams.enabled) {
        return;
    }

    if (plistener) {
        plistener->setProgressStr("PROGRESSBAR_DENOISING");
        plistener->setProgress(0);
    }

    procparams::DenoiseParams denoiseParams = dnparams;
    adjust_params(denoiseParams, scale);

    if (plistener) {
        plistener->setProgress(0.1);
    }

    Imagefloat *calclum = nullptr;
    {
        const int fw = img->getWidth();
        const int fh = img->getHeight();
        // we only need image reduced to 1/4 here
        calclum = new Imagefloat((fw + 1) / 2,
                                 (fh + 1) / 2); // for luminance denoise curve
#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif

        for (int ii = 0; ii < fh; ii += 2) {
            for (int jj = 0; jj < fw; jj += 2) {
                calclum->r(ii >> 1, jj >> 1) = img->r(ii, jj);
                calclum->g(ii >> 1, jj >> 1) = img->g(ii, jj);
                calclum->b(ii >> 1, jj >> 1) = img->b(ii, jj);
            }
        }
        imgsrc->convertColorSpace(calclum, params->icm, currWB);
    }

    ImProcData im(params, scale, multiThread);
    double ecomp = params->exposure.enabled ? params->exposure.expcomp : 0.0;
    ExposureParams expparams;
    expparams.enabled = true;
    expparams.expcomp = ecomp;

    if (ecomp > 0) {
        expcomp(img, &expparams);
    }

    denoise::RGB_denoise(im, img, calclum, denoiseParams);

    if (plistener) {
        plistener->setProgress(0.8);
    }

    if (denoiseParams.smoothingEnabled) {
        denoise::denoiseGuidedSmoothing(im, img);
        if (denoiseParams.nlStrength) {
            img->setMode(Imagefloat::Mode::YUV, multiThread);
            array2D<float> tmp(img->getWidth(), img->getHeight(), img->g.ptrs,
                               ARRAY2D_BYREFERENCE);
            denoise::NLMeans(tmp, 65535.f, denoiseParams.nlStrength,
                             denoiseParams.nlDetail, scale, multiThread);
            img->setMode(Imagefloat::Mode::RGB, multiThread);
        }
    }

    if (ecomp > 0) {
        expparams.expcomp = -ecomp;
        expcomp(img, &expparams);
    }

    if (plistener) {
        plistener->setProgress(1);
    }
}
#define TS 64     // Tile size
#define offset 25 // shift between tiles
// #define fTS ((TS/2+1))  // second dimension of Fourier tiles
#define blkrad 1 // radius of block averaging

// #define epsilon 0.001f/(TS*TS) //tolerance


using namespace denoise;

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

extern const Settings *settings;
extern MyMutex *fftwMutex;

namespace {

template <bool useUpperBound>
void do_median_denoise(float **src, float **dst, float upperBound, int width,
                       int height, denoise::Median medianType, int iterations,
                       int numThreads, float **buffer)
{
    iterations = max(1, iterations);

    typedef denoise::Median Median;

    int border = 1;

    switch (medianType) {
    case Median::TYPE_3X3_SOFT:
    case Median::TYPE_3X3_STRONG: {
        border = 1;
        break;
    }

    case Median::TYPE_5X5_SOFT: {
        border = 2;
        break;
    }

    case Median::TYPE_5X5_STRONG: {
        border = 2;
        break;
    }

    case Median::TYPE_7X7: {
        border = 3;
        break;
    }

    case Median::TYPE_9X9: {
        border = 4;
        break;
    }
    }

    float **allocBuffer = nullptr;
    float **medBuffer[2];
    medBuffer[0] = src;

    // we need a buffer if src == dst or if (src != dst && iterations > 1)
    if (src == dst || iterations > 1) {
        if (buffer == nullptr) { // we didn't get a buffer => create one
            allocBuffer = new float *[height];

            for (int i = 0; i < height; ++i) {
                allocBuffer[i] = new float[width];
            }

            medBuffer[1] = allocBuffer;
        } else { // we got a buffer => use it
            medBuffer[1] = buffer;
        }
    } else { // we can write directly into destination
        medBuffer[1] = dst;
    }

    float **medianIn, **medianOut = nullptr;
    int BufferIndex = 0;

    for (int iteration = 1; iteration <= iterations; ++iteration) {
        medianIn = medBuffer[BufferIndex];
        medianOut = medBuffer[BufferIndex ^ 1];

        if (iteration == 1) { // upper border
            for (int i = 0; i < border; ++i) {
                for (int j = 0; j < width; ++j) {
                    medianOut[i][j] = medianIn[i][j];
                }
            }
        }

#ifdef _OPENMP
#pragma omp parallel for num_threads(numThreads) if (numThreads > 1)           \
    schedule(dynamic, 16)
#endif

        for (int i = border; i < height - border; ++i) {
            int j = 0;

            for (; j < border; ++j) {
                medianOut[i][j] = medianIn[i][j];
            }

            switch (medianType) {
            case Median::TYPE_3X3_SOFT: {
                for (; j < width - border; ++j) {
                    if (!useUpperBound || medianIn[i][j] <= upperBound) {
                        medianOut[i][j] =
                            median(medianIn[i - 1][j], medianIn[i][j - 1],
                                   medianIn[i][j], medianIn[i][j + 1],
                                   medianIn[i + 1][j]);
                    } else {
                        medianOut[i][j] = medianIn[i][j];
                    }
                }

                break;
            }

            case Median::TYPE_3X3_STRONG: {
                for (; j < width - border; ++j) {
                    if (!useUpperBound || medianIn[i][j] <= upperBound) {
                        medianOut[i][j] =
                            median(medianIn[i - 1][j - 1], medianIn[i - 1][j],
                                   medianIn[i - 1][j + 1], medianIn[i][j - 1],
                                   medianIn[i][j], medianIn[i][j + 1],
                                   medianIn[i + 1][j - 1], medianIn[i + 1][j],
                                   medianIn[i + 1][j + 1]);
                    } else {
                        medianOut[i][j] = medianIn[i][j];
                    }
                }

                break;
            }

            case Median::TYPE_5X5_SOFT: {
                for (; j < width - border; ++j) {
                    if (!useUpperBound || medianIn[i][j] <= upperBound) {
                        medianOut[i][j] =
                            median(medianIn[i - 2][j], medianIn[i - 1][j - 1],
                                   medianIn[i - 1][j], medianIn[i - 1][j + 1],
                                   medianIn[i][j - 2], medianIn[i][j - 1],
                                   medianIn[i][j], medianIn[i][j + 1],
                                   medianIn[i][j + 2], medianIn[i + 1][j - 1],
                                   medianIn[i + 1][j], medianIn[i + 1][j + 1],
                                   medianIn[i + 2][j]);
                    } else {
                        medianOut[i][j] = medianIn[i][j];
                    }
                }

                break;
            }

            case Median::TYPE_5X5_STRONG: {
#ifdef ART_SIMD

                for (; !useUpperBound && j < width - border - 3; j += 4) {
                    STVFU(medianOut[i][j],
                          median(LVFU(medianIn[i - 2][j - 2]),
                                 LVFU(medianIn[i - 2][j - 1]),
                                 LVFU(medianIn[i - 2][j]),
                                 LVFU(medianIn[i - 2][j + 1]),
                                 LVFU(medianIn[i - 2][j + 2]),
                                 LVFU(medianIn[i - 1][j - 2]),
                                 LVFU(medianIn[i - 1][j - 1]),
                                 LVFU(medianIn[i - 1][j]),
                                 LVFU(medianIn[i - 1][j + 1]),
                                 LVFU(medianIn[i - 1][j + 2]),
                                 LVFU(medianIn[i][j - 2]),
                                 LVFU(medianIn[i][j - 1]), LVFU(medianIn[i][j]),
                                 LVFU(medianIn[i][j + 1]),
                                 LVFU(medianIn[i][j + 2]),
                                 LVFU(medianIn[i + 1][j - 2]),
                                 LVFU(medianIn[i + 1][j - 1]),
                                 LVFU(medianIn[i + 1][j]),
                                 LVFU(medianIn[i + 1][j + 1]),
                                 LVFU(medianIn[i + 1][j + 2]),
                                 LVFU(medianIn[i + 2][j - 2]),
                                 LVFU(medianIn[i + 2][j - 1]),
                                 LVFU(medianIn[i + 2][j]),
                                 LVFU(medianIn[i + 2][j + 1]),
                                 LVFU(medianIn[i + 2][j + 2])));
                }

#endif

                for (; j < width - border; ++j) {
                    if (!useUpperBound || medianIn[i][j] <= upperBound) {
                        medianOut[i][j] = median(
                            medianIn[i - 2][j - 2], medianIn[i - 2][j - 1],
                            medianIn[i - 2][j], medianIn[i - 2][j + 1],
                            medianIn[i - 2][j + 2], medianIn[i - 1][j - 2],
                            medianIn[i - 1][j - 1], medianIn[i - 1][j],
                            medianIn[i - 1][j + 1], medianIn[i - 1][j + 2],
                            medianIn[i][j - 2], medianIn[i][j - 1],
                            medianIn[i][j], medianIn[i][j + 1],
                            medianIn[i][j + 2], medianIn[i + 1][j - 2],
                            medianIn[i + 1][j - 1], medianIn[i + 1][j],
                            medianIn[i + 1][j + 1], medianIn[i + 1][j + 2],
                            medianIn[i + 2][j - 2], medianIn[i + 2][j - 1],
                            medianIn[i + 2][j], medianIn[i + 2][j + 1],
                            medianIn[i + 2][j + 2]);
                    } else {
                        medianOut[i][j] = medianIn[i][j];
                    }
                }

                break;
            }

            case Median::TYPE_7X7: {
#ifdef ART_SIMD
                std::array<vfloat, 49> vpp ALIGNED16;

                for (; !useUpperBound && j < width - border - 3; j += 4) {
                    for (int kk = 0, ii = -border; ii <= border; ++ii) {
                        for (int jj = -border; jj <= border; ++jj, ++kk) {
                            vpp[kk] = LVFU(medianIn[i + ii][j + jj]);
                        }
                    }

                    STVFU(medianOut[i][j], median(vpp));
                }

#endif

                std::array<float, 49> pp;

                for (; j < width - border; ++j) {
                    if (!useUpperBound || medianIn[i][j] <= upperBound) {
                        for (int kk = 0, ii = -border; ii <= border; ++ii) {
                            for (int jj = -border; jj <= border; ++jj, ++kk) {
                                pp[kk] = medianIn[i + ii][j + jj];
                            }
                        }

                        medianOut[i][j] = median(pp);
                    } else {
                        medianOut[i][j] = medianIn[i][j];
                    }
                }

                break;
            }

            case Median::TYPE_9X9: {
#ifdef ART_SIMD
                std::array<vfloat, 81> vpp ALIGNED16;

                for (; !useUpperBound && j < width - border - 3; j += 4) {
                    for (int kk = 0, ii = -border; ii <= border; ++ii) {
                        for (int jj = -border; jj <= border; ++jj, ++kk) {
                            vpp[kk] = LVFU(medianIn[i + ii][j + jj]);
                        }
                    }

                    STVFU(medianOut[i][j], median(vpp));
                }

#endif

                std::array<float, 81> pp;

                for (; j < width - border; ++j) {
                    if (!useUpperBound || medianIn[i][j] <= upperBound) {
                        for (int kk = 0, ii = -border; ii <= border; ++ii) {
                            for (int jj = -border; jj <= border; ++jj, ++kk) {
                                pp[kk] = medianIn[i + ii][j + jj];
                            }
                        }

                        medianOut[i][j] = median(pp);
                    } else {
                        medianOut[i][j] = medianIn[i][j];
                    }
                }

                for (; j < width; ++j) {
                    medianOut[i][j] = medianIn[i][j];
                }

                break;
            }
            }

            for (; j < width; ++j) {
                medianOut[i][j] = medianIn[i][j];
            }
        }

        if (iteration == 1) { // lower border
            for (int i = height - border; i < height; ++i) {
                for (int j = 0; j < width; ++j) {
                    medianOut[i][j] = medianIn[i][j];
                }
            }
        }

        BufferIndex ^= 1; // swap buffers
    }

    if (medianOut != dst) {
#ifdef _OPENMP
#pragma omp parallel for num_threads(numThreads) if (numThreads > 1)
#endif

        for (int i = 0; i < height; ++i) {
            for (int j = 0; j < width; ++j) {
                dst[i][j] = medianOut[i][j];
            }
        }
    }

    if (allocBuffer != nullptr) { // we allocated memory, so let's free it now
        for (int i = 0; i < height; ++i) {
            delete[] allocBuffer[i];
        }

        delete[] allocBuffer;
    }
}

} // namespace

namespace denoise {

void Median_Denoise(float **src, float **dst, const int width, const int height,
                    const Median medianType, const int iterations,
                    const int numThreads, float **buffer)
{
    do_median_denoise<false>(src, dst, 0.f, width, height, medianType,
                             iterations, numThreads, buffer);
}

void Median_Denoise(float **src, float **dst, float upperBound, const int width,
                    const int height, const Median medianType,
                    const int iterations, const int numThreads, float **buffer)
{
    do_median_denoise<true>(src, dst, upperBound, width, height, medianType,
                            iterations, numThreads, buffer);
}

} // namespace denoise

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

namespace {

void RGBtile_denoise(double scale, float *fLblox, int hblproc,
                     float *noisevar_Ldetail, float *nbrwt,
                     float *blurbuffer) // for DCT
{
    // const int TS = max(int(default_TS / scale), 4);
    // const int offset = max(int(default_offset / scale), 1);

    int blkstart = hblproc * TS * TS;

    const int blur_rad = max(1, int(3 / scale));
    boxabsblur(fLblox + blkstart, nbrwt, blur_rad, blur_rad, TS, TS,
               blurbuffer); // blur neighbor weights for more robust estimation
                            // //for DCT

#ifdef ART_SIMD
    __m128 tempv;
    //__m128  noisevar_Ldetailv = _mm_set1_ps(noisevar_Ldetail);
    __m128 onev = _mm_set1_ps(1.0f);

    for (int n = 0; n < TS * TS; n += 4) { // for DCT
        tempv = onev - xexpf(-SQRV(LVF(nbrwt[n])) /
                             LVF(noisevar_Ldetail[blkstart + n]));
        _mm_storeu_ps(&fLblox[blkstart + n],
                      LVFU(fLblox[blkstart + n]) * tempv);
    } // output neighbor averaged result

#else

    for (int n = 0; n < TS * TS; ++n) { // for DCT
        fLblox[blkstart + n] *=
            (1 - xexpf(-SQR(nbrwt[n]) / noisevar_Ldetail[blkstart + n]));
    } // output neighbor averaged result

#endif

    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
    // printf("vblk=%d  hlk=%d  wsqave=%f   ||   ",vblproc,hblproc,wsqave);

} // end of function tile_denoise

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void RGBoutput_tile_row(double scale, float *bloxrow_L, float **Ldetail,
                        float **tilemask_out, int height, int width, int top)
{
    // const int TS = max(int(default_TS / scale), 4);
    // const int offset = max(int(default_offset / scale), 1);

    const int numblox_W = ceil((static_cast<float>(width)) / (offset));
    const float DCTnorm = 1.0f / (4 * TS * TS); // for DCT

    int imin = MAX(0, -top);
    int bottom = MIN(top + TS, height);
    int imax = bottom - top;

    // add row of tiles to output image
    for (int i = imin; i < imax; ++i) {
        for (int hblk = 0; hblk < numblox_W; ++hblk) {
            int left = (hblk - blkrad) * offset;
            int right = MIN(left + TS, width);
            int jmin = MAX(0, -left);
            int jmax = right - left;
            int indx = hblk * TS;

            for (int j = jmin; j < jmax;
                 ++j) { // this loop gets auto vectorized by gcc
                Ldetail[top + i][left + j] += tilemask_out[i][j] *
                                              bloxrow_L[(indx + i) * TS + j] *
                                              DCTnorm; // for DCT
            }
        }
    }
}
/*
#undef TS
#undef fTS
#undef offset
#undef epsilon
*/

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


} // namespace

namespace denoise {



/* Phase 7's working set: the two tile masks, the FFTW plans and the
 * per-thread blox arrays.
 *
 * Built on first use rather than up front, because FFTW_MEASURE genuinely runs
 * and times candidate transforms -- a call that has no phase 7 at all should
 * not pay for plans it never executes.
 *
 * It also takes both ends of the blox arrays' lifetime, which used to be
 * split: detail_recovery allocated them and RGB_denoise freed them. */
class DctWorkspace {
public:
    DctWorkspace(int imwidth, std::size_t bloxArraySize):
        maxNumbloxW_(ceil((static_cast<float>(imwidth)) / (offset)) +
                     2 * blkrad),
        maskIn_(TS, TS, ARRAY2D_ALIGNED), maskOut_(TS, TS, ARRAY2D_ALIGNED),
        forward_(nullptr), backward_(nullptr), built_(false),
        lblox_(bloxArraySize, nullptr), flblox_(bloxArraySize, nullptr)
    {
    }

    ~DctWorkspace()
    {
        for (std::size_t i = 0; i < lblox_.size(); ++i) {
            if (lblox_[i]) {
                fftwf_free(lblox_[i]);
            }
            if (flblox_[i]) {
                fftwf_free(flblox_[i]);
            }
        }
        /* Guarded, and the members are nullptr-initialised: before this moved
         * here the two plans were plain uninitialised locals destroyed under
         * `if (denoiseLuminance)`, so a lazily-built workspace that was never
         * ensure()d would have destroyed garbage. */
        if (forward_) {
            fftwf_destroy_plan(forward_);
        }
        if (backward_) {
            fftwf_destroy_plan(backward_);
        }
    }

    void ensure()
    {
        if (built_) {
            return;
        }
        built_ = true;
        buildTileMasks();
        buildPlans();
    }

    array2D<float> &maskIn() { return maskIn_; }
    array2D<float> &maskOut() { return maskOut_; }
    fftwf_plan forward() const { return forward_; }
    fftwf_plan backward() const { return backward_; }
    int maxNumbloxW() const { return maxNumbloxW_; }
    float **lblox() { return lblox_.empty() ? nullptr : &lblox_[0]; }
    float **flblox() { return flblox_.empty() ? nullptr : &flblox_[0]; }
    std::size_t bloxArraySize() const { return lblox_.size(); }

private:
    DctWorkspace(const DctWorkspace &);
    DctWorkspace &operator=(const DctWorkspace &);

    void buildTileMasks()
    {
        const int border = MAX(2, TS / 16);
        for (int i = 0; i < TS; ++i) {
            float i1 = abs((i > TS / 2 ? i - TS + 1 : i));
            float vmask =
                (i1 < border ? SQR(sin((rtengine::RT_PI * i1) / (2 * border)))
                             : 1.0f);
            float vmask2 =
                (i1 < 2 * border
                     ? SQR(sin((rtengine::RT_PI * i1) / (2 * border)))
                     : 1.0f);

            for (int j = 0; j < TS; ++j) {
                float j1 = abs((j > TS / 2 ? j - TS + 1 : j));
                maskIn_[i][j] =
                    (vmask * (j1 < border
                                  ? SQR(sin((rtengine::RT_PI * j1) /
                                            (2 * border)))
                                  : 1.0f)) +
                    kEpsilon;
                maskOut_[i][j] =
                    (vmask2 *
                     (j1 < 2 * border
                          ? SQR(sin((rtengine::RT_PI * j1) / (2 * border)))
                          : 1.0f)) +
                    kEpsilon;
            }
        }
    }

    void buildPlans()
    {
        /* FFTW_MEASURE actually runs and times candidate algorithms, so plan
         * creation is real work, not bookkeeping -- worth its own scope. */
        float *Lbloxtmp = reinterpret_cast<float *>(
            fftwf_malloc(maxNumbloxW_ * TS * TS * sizeof(float)));
        float *fLbloxtmp = reinterpret_cast<float *>(
            fftwf_malloc(maxNumbloxW_ * TS * TS * sizeof(float)));

        int nfwd[2] = {TS, TS};

        // for DCT:
        fftw_r2r_kind fwdkind[2] = {FFTW_REDFT10, FFTW_REDFT10};
        fftw_r2r_kind bwdkind[2] = {FFTW_REDFT01, FFTW_REDFT01};

        /* There used to be a second, min_numblox_W-sized pair of plans for the
         * narrower right-edge tile.  With one tile it equals maxNumbloxW_, and
         * detail_recovery's plan_idx (numblox_W != max_numblox_W) was
         * therefore always 0, so the second pair was built with FFTW_MEASURE
         * and never executed. */
        forward_ = fftwf_plan_many_r2r(2, nfwd, maxNumbloxW_, Lbloxtmp, nullptr,
                                       1, TS * TS, fLbloxtmp, nullptr, 1,
                                       TS * TS, fwdkind,
                                       FFTW_MEASURE | FFTW_DESTROY_INPUT);
        backward_ = fftwf_plan_many_r2r(2, nfwd, maxNumbloxW_, fLbloxtmp,
                                        nullptr, 1, TS * TS, Lbloxtmp, nullptr,
                                        1, TS * TS, bwdkind,
                                        FFTW_MEASURE | FFTW_DESTROY_INPUT);
        fftwf_free(Lbloxtmp);
        fftwf_free(fLbloxtmp);
    }

    static const float kEpsilon;

    int maxNumbloxW_;
    array2D<float> maskIn_, maskOut_;
    fftwf_plan forward_, backward_;
    bool built_;
    std::vector<float *> lblox_, flblox_;
};

const float DctWorkspace::kEpsilon = 0.001f / (TS * TS);

namespace {

void laplacian(const array2D<float> &src, array2D<float> &dst, float threshold,
               float ceiling, float factor, bool multiThread)
{
    const int W = src.width();
    const int H = src.height();

    const auto X = [W](int x) -> int {
        return x < 0 ? x + 2 : (x >= W ? x - 2 : x);
    };

    const auto Y = [H](int y) -> int {
        return y < 0 ? y + 2 : (y >= H ? y - 2 : y);
    };

    const auto get = [&src](int y, int x) -> float {
        return std::max(src[y][x], 0.f);
    };

    dst(W, H);
    const float f = factor / ceiling;

#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        int n = Y(y - 1), s = Y(y + 1);
        for (int x = 0; x < W; ++x) {
            int w = X(x - 1), e = X(x + 1);
            float v = -8.f * get(y, x) + get(n, x) + get(s, x) + get(y, w) +
                      get(y, e) + get(n, w) + get(n, e) + get(s, w) + get(s, e);
            dst[y][x] = LIM(std::abs(v) - threshold, 0.f, ceiling) * f;
        }
    }
}

} // namespace

void detail_mask(const array2D<float> &src, array2D<float> &mask, float scaling,
                 float threshold, float ceiling, float factor,
                 BlurType blur_type, float blur, bool multithread)
{
    const int W = src.width();
    const int H = src.height();
    mask(W, H);

    if (W < 8 || H < 8) {
        mask.fill(1.f);
    } else {
        array2D<float> L2(W / 4, H / 4, ARRAY2D_ALIGNED);
        array2D<float> m2(W / 4, H / 4, ARRAY2D_ALIGNED);
        rescaleBilinear(src, L2, multithread);
#ifdef _OPENMP
#pragma omp parallel for if (multithread)
#endif
        for (int y = 0; y < H / 4; ++y) {
            for (int x = 0; x < W / 4; ++x) {
                L2[y][x] = xlin2log(L2[y][x] / scaling, 50.f);
            }
        }
        laplacian(L2, m2, threshold / scaling, ceiling / scaling, factor,
                  multithread);
        rescaleBilinear(m2, mask, multithread);

        const auto scurve = [](float x) -> float {
            constexpr float b = 101.f;
            constexpr float a = 2.23f;
            return xlin2log(pow_F(x, a), b);
        };

        const float thr = 1.f - factor;
#ifdef _OPENMP
#pragma omp parallel for if (multithread)
#endif
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                mask[y][x] = scurve(LIM01(mask[y][x] + thr));
            }
        }

        if (blur_type == BlurType::GAUSS) {
#ifdef _OPENMP
#pragma omp parallel if (multithread)
#endif
            {
                gaussianBlur(mask, mask, W, H, blur);
            }
        } else if (blur_type == BlurType::BOX) {
            if (int(blur) > 0) {
                for (int i = 0; i < 3; ++i) {
                    boxblur(mask, mask, blur, W, H, multithread);
                }
            }
        }
    }
}

/* The detail mask phase 7 modulates its correction with.
 *
 * Leaves `mask` empty when detail_thresh <= 0, which is exactly the condition
 * detailRecovery tests to decide whether to use it. */
void buildDetailMask(int width, int height, LabImage *labdn, int detail_thresh,
                     double scale, array2D<float> &mask)
{
    if (detail_thresh > 0) {
        array2D<float> LL(width, height, labdn->L, ARRAY2D_BYREFERENCE);
        float amount = LIM01(float(detail_thresh) / 100.f);
        detail_mask(LL, mask, 65535.f, 25.f, 10000.f, amount, BlurType::GAUSS,
                    25.f / scale, false);
    }
}

/* Phase 7: the sliding-window block DCT that puts back detail the wavelet
 * shrink removed.  `mask` is buildDetailMask's output, read only when
 * detail_thresh > 0. */
void detailRecovery(int width, int height, LabImage *labdn,
                    array2D<float> *Lin, int numthreads,
                    int denoiseNestedLevels, DctWorkspace &dct,
                    float params_Ldetail, int detail_thresh,
                    const array2D<float> &mask, double scale,
                    bool denoise_aggressive)
{
    dct.ensure();
    /* Aliases, so the block loop below reads exactly as it did when these were
     * eight separate parameters. */
    float **LbloxArray = dct.lblox();
    float **fLbloxArray = dct.flblox();
    array2D<float> &tilemask_in = dct.maskIn();
    array2D<float> &tilemask_out = dct.maskOut();
    const fftwf_plan plan_forward_blox = dct.forward();
    const fftwf_plan plan_backward_blox = dct.backward();
    const int max_numblox_W = dct.maxNumbloxW();

    const auto compute_detail = [](float d) -> float {
        return SQR(static_cast<float>(SQR(100. - d) + 50. * (100. - d)) * TS *
                   0.5f);
    };
    const float detail_hi = compute_detail(params_Ldetail);
    const float detail_lo = compute_detail(0.f);

    // calculation for detail recovery blocks
    const int numblox_W =
        ceil((static_cast<float>(width)) / (offset)) + 2 * blkrad;
    const int numblox_H =
        ceil((static_cast<float>(height)) / (offset)) + 2 * blkrad;

    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
    // Main detail recovery algorithm: Block loop
    // DCT block data storage

    // residual between input and denoised L channel
    array2D<float> Ldetail(width, height, ARRAY2D_CLEAR_DATA | ARRAY2D_ALIGNED);
    array2D<float> totwt(
        width, height,
        ARRAY2D_CLEAR_DATA |
            ARRAY2D_ALIGNED); // weight for combining DCT blocks

    {
        for (int i = 0; i < denoiseNestedLevels * numthreads; ++i) {
            LbloxArray[i] = reinterpret_cast<float *>(
                fftwf_malloc(max_numblox_W * TS * TS * sizeof(float)));
            fLbloxArray[i] = reinterpret_cast<float *>(
                fftwf_malloc(max_numblox_W * TS * TS * sizeof(float)));
        }
    }

#ifdef _OPENMP
    int masterThread = omp_get_thread_num();
#endif
#ifdef _OPENMP
#pragma omp parallel num_threads(                                              \
        denoiseNestedLevels) if (denoiseNestedLevels > 1)
#endif
    {
#ifdef _OPENMP
        int subThread =
            masterThread * denoiseNestedLevels + omp_get_thread_num();
#else
        int subThread = 0;
#endif
        float blurbuffer[TS * TS] ALIGNED64;
        float *Lblox = LbloxArray[subThread];
        float *fLblox = fLbloxArray[subThread];
        float pBuf[width + TS + 2 * blkrad * offset] ALIGNED16;
        float nbrwt[TS * TS] ALIGNED64;
        AlignedBuffer<float> detail_factor_buf(numblox_W * TS * TS);
        float *detail_factor = detail_factor_buf.data;

        /* Block rows overlap: consecutive vblk tops are `offset` (25) apart
         * while each block spans TS (64) rows, so a plain `omp for` over vblk
         * had several threads doing `+=` into the same Ldetail/totwt elements
         * at once.  That is a data race and therefore UB, even though in
         * practice the lost updates were small enough not to be the source of
         * denoise's ~1 LSB run-to-run scatter -- disabling this phase
         * entirely does not make the output deterministic, so that scatter
         * comes from somewhere else in the wavelet path and is still open.
         *
         * Fixed by colouring the loop instead of guarding it: blocks whose
         * indices differ by kPhases = ceil(TS / offset) = 3 are at least
         * 3*25 = 75 >= 64 rows apart and therefore cannot overlap, so the
         * three residue classes can each be distributed across threads and
         * separated by the implicit barrier at the end of `omp for`.  No
         * atomics (which would remove the race but not the nondeterminism,
         * since float addition order would still vary), no per-thread
         * accumulators (10 threads x 2 x W x H floats is ~2 GB at 24 Mpix),
         * and no loss of parallelism worth measuring: numblox_H/3 is still
         * dozens of blocks per round. */
        constexpr int kPhases = (TS + offset - 1) / offset;
        static_assert(kPhases * offset >= TS,
                      "block rows in the same phase class must not overlap");

        for (int phase = 0; phase < kPhases; ++phase) {
#ifdef _OPENMP
#pragma omp for
#endif

            for (int vblk = phase; vblk < numblox_H; vblk += kPhases) {

                int top = (vblk - blkrad) * offset;
                float *datarow = pBuf + blkrad * offset;

                for (int i = 0; i < TS; ++i) {
                    int row = top + i;
                    int rr = row;

                    if (row < 0) {
                        rr = MIN(-row, height - 1);
                    } else if (row >= height) {
                        rr = MAX(0, 2 * height - 2 - row);
                    }

                    for (int j = 0; j < labdn->W; ++j) {
                        datarow[j] = ((*Lin)[rr][j] - labdn->L[rr][j]);
                    }

                    for (int j = -blkrad * offset; j < 0; ++j) {
                        datarow[j] = datarow[MIN(-j, width - 1)];
                    }

                    for (int j = width; j < width + TS + blkrad * offset; ++j) {
                        datarow[j] = datarow[MAX(0, 2 * width - 2 - j)];
                    } // now we have a padded data row

                    // now fill this row of the blocks with Lab high pass data
                    for (int hblk = 0; hblk < numblox_W; ++hblk) {
                        int left = (hblk - blkrad) * offset;
                        int indx = (hblk)*TS; // index of block in malloc

                        if (top + i >= 0 && top + i < height) {
                            int j;

                            for (j = 0; j < min((-left), TS); ++j) {
                                Lblox[(indx + i) * TS + j] =
                                    tilemask_in[i][j] *
                                    datarow[left + j]; // luma data
                                detail_factor[(indx + i) * TS + j] = detail_lo;
                            }

                            for (; j < min(TS, width - left); ++j) {
                                Lblox[(indx + i) * TS + j] =
                                    tilemask_in[i][j] *
                                    datarow[left + j]; // luma data
                                totwt[top + i][left + j] +=
                                    tilemask_in[i][j] * tilemask_out[i][j];
                                detail_factor[(indx + i) * TS + j] =
                                    detail_thresh > 0
                                        ? compute_detail(params_Ldetail *
                                                         mask[top + i][left + j])
                                        : detail_hi;
                            }

                            for (; j < TS; ++j) {
                                Lblox[(indx + i) * TS + j] =
                                    tilemask_in[i][j] *
                                    datarow[left + j]; // luma data
                                detail_factor[(indx + i) * TS + j] = detail_lo;
                            }
                        } else {
                            for (int j = 0; j < TS; ++j) {
                                Lblox[(indx + i) * TS + j] =
                                    tilemask_in[i][j] *
                                    datarow[left + j]; // luma data
                                detail_factor[(indx + i) * TS + j] = detail_lo;
                            }
                        }
                    }

                } // end of filling block row

                //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
                // fftwf_print_plan (plan_forward_blox);
                fftwf_execute_r2r(plan_forward_blox, Lblox,
                                  fLblox); // DCT an entire row of tiles
                //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
                // now process the vblk row of blocks for noise reduction
                for (int hblk = 0; hblk < numblox_W; ++hblk) {
                    RGBtile_denoise(scale, fLblox, hblk, detail_factor, nbrwt,
                                    blurbuffer);
                } // end of horizontal block loop

                //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

                // now perform inverse FT of an entire row of blocks
                fftwf_execute_r2r(plan_backward_blox, fLblox,
                                  fLblox); // for DCT
                int topproc = (vblk - blkrad) * offset;
                // add row of blocks to output image tile
                RGBoutput_tile_row(scale, fLblox, Ldetail, tilemask_out, height,
                                   width, topproc);
                //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            } // end of vertical block loop
        } // end of phase class

        //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
    }
    //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

#ifdef _OPENMP
#pragma omp parallel for num_threads(                                          \
        denoiseNestedLevels) if (denoiseNestedLevels > 1)
#endif

    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            labdn->L[i][j] +=
                Ldetail[i][j] / totwt[i][j]; // note that labdn initially stores
                                             // the denoised hipass data
        }
    }
}



/* Phase 1a/1c fused, on the host: classify each half-resolution sample's
 * chroma against the noise curve and expand the result -- plus a flat luma
 * term -- into the quarter-resolution maps phases 4 and 5 read. */
/* `calclum` is a quarter-resolution copy of the source image, taken and
 * colour-converted before denoising touches anything (ImProcFunctions::denoise
 * builds it, RGB_denoise below owns and frees it) -- exactly the resolution of
 * the noisevar maps, so no further subsampling is needed here. */
void computeNoisevarMaps(const DenoiseContext &c, Imagefloat *calclum,
                         const denoise::NoiseCurve &noiseCCurve)
{
    const float maxNoiseVarab = max(c.noisevarab_b, c.noisevarab_r);
    const int H2 = (c.H + 1) / 2;
    const float cn100Precalc = SQR(1.f + 4.f * noiseCCurve[100.f / 60.f]);
    const float (*wpi)[3] = c.wpi;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 16) num_threads(                    \
        c.denoiseNestedLevels) if (c.denoiseNestedLevels > 1)
#endif
    for (int ii = 0; ii < H2; ++ii) {
        for (int jj = 0; jj < c.W2; ++jj) {
            float LLum, AAum, BBum;
            const float RL = calclum->r(ii, jj);
            const float GL = calclum->g(ii, jj);
            const float BL = calclum->b(ii, jj);
            // determine luminance and chrominance for noisecurves
            float XL, YL, ZL;
            Color::rgbxyz(RL, GL, BL, XL, YL, ZL, wpi);
            Color::XYZ2Lab(XL, YL, ZL, LLum, AAum, BBum);

            const float cN = sqrtf(SQR(AAum) + SQR(BBum));
            const float ccalcVal = cN > 100
                                       ? SQR(1.f + 4.f * noiseCCurve[cN / 60.f])
                                       : cn100Precalc;

            const int k = ii * c.W2 + jj;
            c.noisevarlum[k] = c.noisevarL;
            c.noisevarchrom[k] = maxNoiseVarab * ccalcVal;
        }
    }
}

/* Phase 1b.  Pointwise: optional inverse "denoise gamma" (Lab mode only), the
 * forward gamma LUT, then RGB -> YUV (or Lab) with the working-space matrix. */
void denoiseFill(const DenoiseContext &c, Imagefloat *src, LabImage *labdn)
{
#ifdef _OPENMP
#pragma omp parallel for num_threads(                                          \
        c.denoiseNestedLevels) if (c.denoiseNestedLevels > 1)
#endif
    for (int i = 0; i < c.H; ++i) {
        for (int j = 0; j < c.W; ++j) {
            float X = src->r(i, j);
            float Y = src->g(i, j);
            float Z = src->b(i, j);

            if (c.lab_mode) {
                X = Color::denoiseIGammaTab[X];
                Y = Color::denoiseIGammaTab[Y];
                Z = Color::denoiseIGammaTab[Z];
            }

            // conversion colorspace to determine luminance with no gamma
            X = c.applyGamma(X);
            Y = c.applyGamma(Y);
            Z = c.applyGamma(Z);

            float l, u, v;
            if (c.lab_mode) {
                Color::rgb2lab(X, Y, Z, l, v, u, c.wpi);
            } else {
                Color::rgb2yuv(X, Y, Z, l, u, v, c.wpi);
            }
            labdn->L[i][j] = l;
            labdn->a[i][j] = v;
            labdn->b[i][j] = u;
        }
    }
}

/* Phase 8.  The inverse of phase 1, plus the chroma boost that phase 1 has no
 * counterpart for: coefficients whose chroma magnitude exceeds 3000 get pushed
 * outwards by qhighFactor * realred/realblue, which is how the aggressive
 * (QUALITY_HIGH) mode restores saturation the shrinkage removed. */
void denoiseOutput(const DenoiseContext &c, LabImage *labdn, Imagefloat *dst,
                   float qhighFactor, float realred, float realblue)
{
#ifdef _OPENMP
#pragma omp parallel for num_threads(c.denoiseNestedLevels)
#endif
    for (int i = 0; i < c.H; ++i) {
        for (int j = 0; j < c.W; ++j) {
            const float c_h =
                sqrt(SQR(labdn->a[i][j]) + SQR(labdn->b[i][j]));

            if (c_h > 3000.f) {
                labdn->a[i][j] *= 1.f + qhighFactor * realred / 100.f;
                labdn->b[i][j] *= 1.f + qhighFactor * realblue / 100.f;
            }

            float X, Y, Z;
            if (c.lab_mode) {
                Color::lab2rgb(labdn->L[i][j], labdn->a[i][j], labdn->b[i][j],
                               X, Y, Z, c.wpi_inverse);
            } else {
                Color::yuv2rgb(labdn->L[i][j], labdn->b[i][j], labdn->a[i][j],
                               X, Y, Z, c.wpi);
            }

            X = c.applyIGamma(X);
            Y = c.applyIGamma(Y);
            Z = c.applyIGamma(Z);

            if (c.lab_mode) {
                X = Color::denoiseGammaTab[X];
                Y = Color::denoiseGammaTab[Y];
                Z = Color::denoiseGammaTab[Z];
            }

            dst->r(i, j) = X;
            dst->g(i, j) = Y;
            dst->b(i, j) = Z;
        }
    }
}


/* Everything RGB_denoise derives before it knows -- or cares -- which backend
 * runs the eight phases: the modes, the strengths, the gamma tables, the
 * working-space matrices and the quarter-resolution noise maps.
 *
 * Non-copyable on purpose: `ctx` holds pointers into this object's own
 * members, so a copy would alias the wrong tables and a temporary would
 * dangle.  finalize() sets those pointers and must run after the members it
 * points at are filled. */
struct DenoisePrep {
    DenoisePrep():
        W(0), H(0), scale(1.0), nrQuality(QUALITY_STANDARD), autoch(false),
        denoiseLuminance(false), lab_mode(false), useNoiseCCurve(true),
        qhighFactor(1.f), noisevarL(0.f), noisevarab_r(0.f), noisevarab_b(0.f),
        realred(0.f), realblue(0.f), params_Ldetail(0.f), detail_thresh(0),
        gam(1.f), gamthresh(0.001f), gamslope(1.f), igamthresh(0.001f),
        igamslope(1.f), gamcurve(65536, LUT_CLIP_BELOW),
        igamcurve(65536, LUT_CLIP_BELOW), levwav(0), numthreads(1),
        denoiseNestedLevels(1)
    {
    }

    int W, H;
    double scale;
    nrquality nrQuality;
    bool autoch, denoiseLuminance, lab_mode, useNoiseCCurve;
    float qhighFactor;

    float noisevarL, noisevarab_r, noisevarab_b;
    float realred, realblue;

    float params_Ldetail;      // phase 7
    int detail_thresh;         // phase 7

    float wpi[3][3], wpi_inverse[3][3];

    float gam, gamthresh, gamslope, igamthresh, igamslope;
    LUTf gamcurve, igamcurve;

    /* The chroma noise curve RGB_denoise's phase 1 classifies half-resolution
     * samples against.  One fixed curve, built once here -- construction is
     * O(1), not O(image) -- and read by computeNoisevarMaps below. */
    denoise::NoiseCurve noiseCCurve;

    /* The quarter-resolution maps phases 4 and 5 read.  Backing store for
     * ctx.noisevarlum/noisevarchrom, sized here so those pointers are never
     * null, but left uninitialized: computeNoisevarMaps is what actually
     * writes them. */
    std::vector<float> lumcalcBuf, ccalcBuf;

    int levwav;                // waveletLevels(), computed once
    int numthreads;
    int denoiseNestedLevels;   // nested OpenMP thread count for the phases'
                               // inner parallel regions

    DenoiseContext ctx;

    void finalize()
    {
        ctx.W = W;
        ctx.H = H;
        ctx.W2 = (W + 1) / 2;
        ctx.scale = scale;
        ctx.lab_mode = lab_mode;
        ctx.useNoiseCCurve = useNoiseCCurve;
        ctx.noisevarL = noisevarL;
        ctx.noisevarab_r = noisevarab_r;
        ctx.noisevarab_b = noisevarab_b;
        ctx.wpi = wpi;
        ctx.wpi_inverse = wpi_inverse;
        ctx.gam = gam;
        ctx.gamthresh = gamthresh;
        ctx.gamslope = gamslope;
        ctx.igamthresh = igamthresh;
        ctx.igamslope = igamslope;
        ctx.gamcurve = &gamcurve;
        ctx.igamcurve = &igamcurve;
        ctx.noisevarlum = lumcalcBuf.empty() ? nullptr : &lumcalcBuf[0];
        ctx.noisevarchrom = ccalcBuf.empty() ? nullptr : &ccalcBuf[0];
        /* Both buffers are sized (below in denoisePrepare) but not yet
         * filled; computeNoisevarMaps or a device fill still has to run
         * before either is read. */
        ctx.denoiseNestedLevels = denoiseNestedLevels;
    }

private:
    DenoisePrep(const DenoisePrep &);
    DenoisePrep &operator=(const DenoisePrep &);
};

/* Fills `p` from the parameters and the source pixels.  False means there is
 * nothing to denoise and RGB_denoise should not run.
 *
 * Reads src->r/g/b through the planar accessors to build the chroma noise
 * map, so the caller must have brought the image to the host first. */
bool denoisePrepare(ImProcData &im, Imagefloat *src,
                    const procparams::DenoiseParams &dnparams, DenoisePrep &p)
{
    const ProcParams *params = im.params;

    p.noiseCCurve.Set(
        {FCT_MinMaxCPoints, 0.05, 0.50, 0.35, 0.35, 0.35, 0.05, 0.35, 0.35});

    p.scale = im.scale;
    p.nrQuality = (!dnparams.aggressive) ? QUALITY_STANDARD : QUALITY_HIGH;
    p.qhighFactor = (p.nrQuality == QUALITY_HIGH)
                        ? 1.f / static_cast<float>(0.9 /*settings->nrhigh*/)
                        : 1.0f;
    p.useNoiseCCurve = true;
    p.autoch = dnparams.chrominanceMethod ==
               procparams::DenoiseParams::ChrominanceMethod::AUTOMATIC;
    p.lab_mode =
        dnparams.colorSpace == procparams::DenoiseParams::ColorSpace::LAB;

    // init luma noisevarL
    const float noiseluma = static_cast<float>(dnparams.luminance);
    p.noisevarL =
        static_cast<float>(SQR((noiseluma / 125.0) * (1.0 + noiseluma / 25.0)));
    p.denoiseLuminance = (p.noisevarL > 0.00001f);

    TMatrix wprofi =
        ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    TMatrix wprofi_inverse = ICCStore::getInstance()->workingSpaceInverseMatrix(
        params->icm.workingProfile);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            p.wpi[i][j] = static_cast<float>(wprofi[i][j]);
            p.wpi_inverse[i][j] = static_cast<float>(wprofi_inverse[i][j]);
        }
    }

    if (p.useNoiseCCurve) {
        /* Sized here so ctx.noisevarlum/noisevarchrom are never null (see
         * the note on DenoisePrep), but not filled: that O(image) work is
         * computeNoisevarMaps's own, below. */
        const int wid = (src->getWidth() + 1) / 2;
        const int hei = (src->getHeight() + 1) / 2;
        p.lumcalcBuf.assign(std::size_t(hei) * wid, 0.f);
        p.ccalcBuf.assign(std::size_t(hei) * wid, 0.f);
    }

    p.H = src->getHeight();
    p.W = src->getWidth();

    if (dnparams.luminance == 0 && dnparams.chrominance == 0) {
        return false;
    }

    // gamma transform for input data
    p.gam = dnparams.gamma;
    p.gamthresh = 0.001f;
    p.gamslope =
        exp(log(static_cast<double>(p.gamthresh)) / p.gam) / p.gamthresh;
    Color::gammaf2lut(p.gamcurve, p.gam, p.gamthresh, p.gamslope, 65535.f,
                      65535.f);

    // inverse gamma transform for output data
    const float igam = 1.f / p.gam;
    p.igamthresh = p.gamthresh * p.gamslope;
    p.igamslope = 1.f / p.gamslope;
    Color::gammaf2lut(p.igamcurve, igam, p.igamthresh, p.igamslope, 65535.f,
                      65535.f);

    // max out to avoid div by zero when using noisevar_Ldetail as divisor
    p.params_Ldetail = min(float(dnparams.luminanceDetail), 99.9f);
    p.detail_thresh = dnparams.luminanceDetailThreshold;

    /* The two `ponder` overrides that used to sit here -- one reading
     * ch_M/max_r/max_b, one zeroing the chroma strengths -- were both gated on
     * a hardcoded `false`.  They are the sole readers of ch_M/max_r/max_b,
     * which is why those parameters are now unused (see the note on
     * RGB_denoise's signature). */
    const float interm_med = static_cast<float>(dnparams.chrominance) / 10.0;
    // increase slower than linear for more sensitivity below zero
    const float intermred = dnparams.chrominanceRedGreen > 0.
                                ? float(dnparams.chrominanceRedGreen / 10.)
                                : float(dnparams.chrominanceRedGreen) / 7.0f;
    const float intermblue = dnparams.chrominanceBlueYellow > 0.
                                 ? float(dnparams.chrominanceBlueYellow / 10.)
                                 : float(dnparams.chrominanceBlueYellow) / 7.0f;

    p.realred = interm_med + intermred;
    if (p.realred <= 0.f) {
        p.realred = 0.001f;
    }
    p.realblue = interm_med + intermblue;
    if (p.realblue <= 0.f) {
        p.realblue = 0.001f;
    }
    p.noisevarab_r = SQR(p.realred);
    p.noisevarab_b = SQR(p.realblue);

    p.levwav =
        waveletLevels(p.realred, p.realblue, p.nrQuality, p.scale, p.W, p.H);

    p.numthreads = 1;

    /* p.denoiseNestedLevels is read by every phase's num_threads(...) pragma
     * here and in wavelet.cc, so it has to be set before any phase runs --
     * and only on a call that actually denoises, which is why this sits
     * after the early return above rather than at the top. */
#ifdef _OPENMP
    p.denoiseNestedLevels = omp_get_num_procs() / p.numthreads;

    if (p.denoiseNestedLevels < 2) {
        p.denoiseNestedLevels = 1;
    }

    if (options.rgbDenoiseThreadLimit > 0)
        while (p.denoiseNestedLevels * p.numthreads >
               options.rgbDenoiseThreadLimit) {
            p.denoiseNestedLevels--;
        }

    if (settings->verbose) {
        printf("RGB_denoise uses %d thread(s)\n", p.denoiseNestedLevels);
    }
#endif // _OPENMP

    p.finalize();
    return true;
}

/* RGB_denoise's eight phases (see the comment above DenoiseContext), run in
 * order on `prep`, which denoisePrepare has already filled in. */
void runDenoisePhases(Imagefloat *src, Imagefloat *calclum,
                      const DenoisePrep &prep)
{
    Imagefloat *dst = src;
    const DenoiseContext &ctx = prep.ctx;
    const int width = prep.W, height = prep.H;

    DctWorkspace dct(width,
                     std::size_t(prep.denoiseNestedLevels) * prep.numthreads);

    array2D<float> *Lin = nullptr;              // input L channel
    LabImage *labdn = new LabImage(width, height);  // wavelet denoised image

    {
        computeNoisevarMaps(ctx, calclum, prep.noiseCCurve);
        denoiseFill(ctx, src, labdn);
    }

    denoiseWavelet(ctx, labdn, Lin, prep.levwav, prep.nrQuality, prep.autoch,
                   prep.denoiseLuminance); // phases 2-6

    if (prep.denoiseLuminance) {
        // now do detail recovery using block DCT to detect patterns missed by
        // wavelet denoise; blocks are not the same thing as tiles!
        array2D<float> mask(ARRAY2D_ALIGNED);
        buildDetailMask(width, height, labdn, prep.detail_thresh, prep.scale,
                        mask);
        detailRecovery(width, height, labdn, Lin, prep.numthreads,
                       prep.denoiseNestedLevels, dct, prep.params_Ldetail,
                       prep.detail_thresh, mask, prep.scale,
                       prep.nrQuality == QUALITY_HIGH);
    }

    // Phase 8: inverse colour transform back to RGB.
    {
        denoiseOutput(ctx, labdn, dst, prep.qhighFactor, prep.realred,
                      prep.realblue);
    }

    delete labdn;
    delete Lin;
}

void RGB_denoise(ImProcData &im, Imagefloat *src, Imagefloat *calclum,
                 const procparams::DenoiseParams &dnparams)
{
    BENCHFUN
    MyTime t1e, t2e;
    t1e.set();

    MyMutex::MyLock lock(*fftwMutex);

    /* denoisePrepare returning false means there is nothing to denoise.  The
     * timing line below still prints in that case, as it always has. */
    DenoisePrep prep;
    if (denoisePrepare(im, src, dnparams, prep)) {
        runDenoisePhases(src, calclum, prep);
    }

    delete calclum;

    // #ifdef _DEBUG
    if (settings->verbose) {
        t2e.set();
        printf("Denoise performed in %d usec:\n", t2e.etime(t1e));
    }
    // #endif
}

#undef TS
#undef offset
#undef blkrad

} // namespace denoise


} // namespace rtengine
