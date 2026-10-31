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

#include "iccstore.h"
#include "improcfun.h"
#include "watermark.h"
#include <cmath>
#include <cstdint>
#include <iostream>

namespace art { namespace engine {

namespace {

inline float srgb_encode(float v)
{
    const float a = std::abs(v);
    const float e = a <= 0.0031308f ? a * 12.92f
                                    : 1.055f * std::pow(a, 1.f / 2.4f) - 0.055f;
    return v < 0.f ? -e : e;
}

inline float srgb_decode(float v)
{
    const float a = std::abs(v);
    const float d =
        a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return v < 0.f ? -d : d;
}

void matmul(const float (*a)[3], const float (*b)[3], float (*res)[3])
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            res[i][j] = 0.f;
            for (int k = 0; k < 3; ++k) {
                res[i][j] += a[i][k] * b[k][j];
            }
        }
    }
}

inline void mat_apply(const float (*m)[3], float &r, float &g, float &b)
{
    const float rr = m[0][0] * r + m[0][1] * g + m[0][2] * b;
    const float gg = m[1][0] * r + m[1][1] * g + m[1][2] * b;
    const float bb = m[2][0] * r + m[2][1] * g + m[2][2] * b;
    r = rr;
    g = gg;
    b = bb;
}

}

void ImProcFunctions::applyWatermark(Imagefloat *img, int pre_border_w,
                                     int pre_border_h)
{
    const auto &wp = params->watermark;
    if (!wp.enabled || !img) {
        return;
    }

    const int W = img->getWidth();
    const int H = img->getHeight();

    double rx = 0, ry = 0, rw = W, rh = H;
    const auto &bp = params->border;
    if (wp.reference == procparams::WatermarkParams::Reference::IMAGE &&
        bp.enabled &&
        bp.placement == procparams::BorderParams::Placement::OUTSIDE &&
        pre_border_w > 0 && pre_border_h > 0) {
        double side, bottom;
        bp.getWidths(pre_border_w, pre_border_h, 1.0, side, bottom);
        const int s = std::max(int(std::round(side)), 0);
        rx = s;
        ry = s;
        rw = pre_border_w;
        rh = pre_border_h;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        std::cerr << "WARNING: watermark: out of memory" << std::endl;
        return;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_translate(cr, rx, ry);
    Glib::ustring error;
    const bool ok = drawWatermark(cr, wp, rw, rh, &error);
    cairo_destroy(cr);

    if (!ok) {
        std::cerr << "WARNING: watermark not applied: " << error << std::endl;
        cairo_surface_destroy(surf);
        return;
    }
    cairo_surface_flush(surf);

    float to_srgb[3][3], from_srgb[3][3];
    {
        auto iccs = ICCStore::getInstance();
        TMatrix ws = iccs->workingSpaceMatrix(img->colorSpace());
        TMatrix iws = iccs->workingSpaceInverseMatrix(img->colorSpace());
        TMatrix srgb = iccs->workingSpaceMatrix("sRGB");
        TMatrix isrgb = iccs->workingSpaceInverseMatrix("sRGB");
        matmul(isrgb, ws, to_srgb);
        matmul(iws, srgb, from_srgb);
    }

    img->setMode(Imagefloat::Mode::RGB, multiThread);
    img->syncCpuForWrite();
    const float mul = img->scale() == Imagefloat::Scale::UNIT ? 1.f : 65535.f;

    const unsigned char *data = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);

#ifdef _OPENMP
#pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
        for (int x = 0; x < W; ++x) {
            const uint32_t px = row[x];
            const uint32_t a8 = px >> 24;
            if (a8 == 0) {
                continue;
            }
            const float a = a8 / 255.f;
            const float pr = ((px >> 16) & 0xff) / 255.f;
            const float pg = ((px >> 8) & 0xff) / 255.f;
            const float pb = (px & 0xff) / 255.f;

            float r = img->r(y, x) / mul;
            float g = img->g(y, x) / mul;
            float b = img->b(y, x) / mul;
            mat_apply(to_srgb, r, g, b);
            r = srgb_decode(srgb_encode(r) * (1.f - a) + pr);
            g = srgb_decode(srgb_encode(g) * (1.f - a) + pg);
            b = srgb_decode(srgb_encode(b) * (1.f - a) + pb);
            mat_apply(from_srgb, r, g, b);

            img->r(y, x) = r * mul;
            img->g(y, x) = g * mul;
            img->b(y, x) = b * mul;
        }
    }

    cairo_surface_destroy(surf);
}

}}
