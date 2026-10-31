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
#pragma once

#include "procparams.h"
#include <cairo.h>

namespace art { namespace engine {

bool drawWatermark(cairo_t *cr, const procparams::WatermarkParams &wp,
                   double ref_w, double ref_h, Glib::ustring *error = nullptr);

bool checkWatermarkSource(const procparams::WatermarkParams &wp,
                          Glib::ustring &error);

struct WatermarkBox {
    double cx, cy;
    double w, h;
    double anchor_x, anchor_y;
    double point_x, point_y;
};

bool getWatermarkBox(const procparams::WatermarkParams &wp, double ref_w,
                     double ref_h, WatermarkBox &box);

inline double watermarkPercentBase(double ref_w, double ref_h)
{
    return std::min(ref_w, ref_h);
}

void watermarkAnchorPoint(procparams::WatermarkParams::Anchor a, double ref_w,
                          double ref_h, double &x, double &y);

void watermarkPositionFor(procparams::WatermarkParams::Anchor a, double ref_w,
                          double ref_h, double bw, double bh, double cx,
                          double cy, double &pos_x, double &pos_y);

struct WatermarkFrame {
    double image_x, image_y, image_w, image_h;
    double canvas_x, canvas_y, canvas_w, canvas_h;
    double ref_x, ref_y, ref_w, ref_h;
};

bool getWatermarkFrame(const procparams::CropParams &crop,
                       const procparams::BorderParams &border,
                       const procparams::WatermarkParams &wp, int full_w,
                       int full_h, double out_scale, WatermarkFrame &frame);

}}
