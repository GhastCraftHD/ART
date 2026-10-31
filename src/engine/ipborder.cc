/* -*- C++ -*-
 *
 *  This file is part of ART.
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

#ifdef _OPENMP
#include <omp.h>
#endif

#include "color.h"
#include "iccstore.h"
#include "improcfun.h"
#include <cmath>

namespace art { namespace engine {

namespace {

inline double srgb_to_linear(double v)
{
    v = std::max(0.0, std::min(1.0, v));
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

void border_color(const procparams::BorderParams &bp, const Imagefloat *img,
                  float &r, float &g, float &b)
{
    const float lr = srgb_to_linear(bp.red);
    const float lg = srgb_to_linear(bp.green);
    const float lb = srgb_to_linear(bp.blue);

    float X, Y, Z;
    Color::rgbxyz(lr, lg, lb, X, Y, Z,
                  ICCStore::getInstance()->workingSpaceMatrix("sRGB"));
    Color::xyz2rgb(X, Y, Z, r, g, b,
                   ICCStore::getInstance()->workingSpaceInverseMatrix(
                       img->colorSpace()));

    const float mul = img->scale() == Imagefloat::Scale::UNIT ? 1.f : 65535.f;
    r *= mul;
    g *= mul;
    b *= mul;
}

}

Imagefloat *ImProcFunctions::applyBorder(Imagefloat *img)
{
    const auto &bp = params->border;
    if (!bp.enabled || !img) {
        return img;
    }

    const int W = img->getWidth();
    const int H = img->getHeight();

    double side_d, bottom_d;
    bp.getWidths(W, H, 1.0, side_d, bottom_d);
    const int side = std::max(int(std::round(side_d)), 0);
    const int bottom = std::max(int(std::round(bottom_d)), 0);

    if (side == 0 && bottom == 0) {
        return img;
    }

    img->setMode(Imagefloat::Mode::RGB, multiThread);

    float cr, cg, cb;
    border_color(bp, img, cr, cg, cb);

    if (bp.placement == procparams::BorderParams::Placement::INSIDE) {
        img->syncCpuForWrite();

        const int y_bottom = std::max(H - bottom, side);

#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif
        for (int y = 0; y < H; ++y) {
            const bool full_row = y < side || y >= y_bottom;
            for (int x = 0; x < W; ++x) {
                if (full_row || x < side || x >= W - side) {
                    img->r(y, x) = cr;
                    img->g(y, x) = cg;
                    img->b(y, x) = cb;
                }
            }
        }

        return img;
    }

    const int nW = W + 2 * side;
    const int nH = H + side + bottom;

    img->syncCpu();
    Imagefloat *res = new Imagefloat(nW, nH, img);
    res->assignMode(Imagefloat::Mode::RGB);
    res->syncCpuForWrite();

#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < nH; ++y) {
        const int sy = y - side;
        const bool in_rows = sy >= 0 && sy < H;
        for (int x = 0; x < nW; ++x) {
            const int sx = x - side;
            if (in_rows && sx >= 0 && sx < W) {
                res->r(y, x) = img->r(sy, sx);
                res->g(y, x) = img->g(sy, sx);
                res->b(y, x) = img->b(sy, sx);
            } else {
                res->r(y, x) = cr;
                res->g(y, x) = cg;
                res->b(y, x) = cb;
            }
        }
    }

    delete img;
    return res;
}

}}
