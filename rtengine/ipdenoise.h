/* -*- C++ -*-
 *
 *  This file is part of ART.
 *
 *  Copyright 2019 Alberto Griggio <alberto.griggio@gmail.com>
 *
 *  ART is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  ART is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with ART.  If not, see <http://www.gnu.org/licenses/>.
 */
// extracted and datapted from ImProcFunctions (improcfun.cc, ipdenoise.cc) of
// RawTherapee

#pragma once

#include "curves.h"
#include "improcfun.h"

namespace rtengine {
namespace denoise {

class NoiseCurve {
private:
    LUTf lutNoiseCurve; // 0xffff range
    float sum;
    void Set(const Curve &pCurve);

public:
    virtual ~NoiseCurve() {};
    NoiseCurve();
    void Reset();
    void Set(const std::vector<double> &curvePoints);

    float getSum() const { return sum; }
    float operator[](float index) const { return lutNoiseCurve[index]; }
    operator bool(void) const { return lutNoiseCurve; }
};

void denoiseGuidedSmoothing(ImProcData &im, Imagefloat *rgb);

void RGB_denoise(ImProcData &im, Imagefloat *src, Imagefloat *calclum,
                 const procparams::DenoiseParams &dnparams);

enum class Median {
    TYPE_3X3_SOFT,
    TYPE_3X3_STRONG,
    TYPE_5X5_SOFT,
    TYPE_5X5_STRONG,
    TYPE_7X7,
    TYPE_9X9
};

void Median_Denoise(float **src, float **dst, float upperBound, int width,
                    int height, Median medianType, int iterations,
                    int numThreads, float **buffer = nullptr);

void Median_Denoise(float **src, float **dst, int width, int height,
                    Median medianType, int iterations, int numThreads,
                    float **buffer = nullptr);

enum class BlurType { OFF, BOX, GAUSS };
void detail_mask(const array2D<float> &src, array2D<float> &mask, float scaling,
                 float threshold, float ceiling, float factor, BlurType blur,
                 float blur_radius, bool multithread);

void NLMeans(array2D<float> &img, float normcoeff, int strength,
             int detail_thresh, float scale, bool multithread);

} // namespace denoise
} // namespace rtengine
