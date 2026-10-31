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

#include "watermark.h"

#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>

namespace art { namespace engine {

using procparams::WatermarkParams;

namespace {

struct Source {
    cairo_surface_t *surface = nullptr;
    RsvgHandle *svg = nullptr;
    double width = 0;
    double height = 0;

    ~Source()
    {
        if (surface) {
            cairo_surface_destroy(surface);
        }
        if (svg) {
            g_object_unref(svg);
        }
    }
};

struct CacheEntry {
    std::string fname;
    bool is_svg;
    gint64 mtime;
    std::shared_ptr<Source> source;
};

constexpr std::size_t CACHE_SIZE = 4;
std::mutex cache_mutex;
std::list<CacheEntry> cache;

std::mutex svg_render_mutex;

gint64 file_mtime(const std::string &fname)
{
    GStatBuf st;
    if (g_stat(fname.c_str(), &st) != 0) {
        return -1;
    }
    return gint64(st.st_mtime);
}

std::shared_ptr<Source> load_image(const std::string &fname,
                                   Glib::ustring &error)
{
    GError *err = nullptr;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(fname.c_str(), &err);
    if (!pb) {
        error = err ? err->message : "unknown error";
        if (err) {
            g_error_free(err);
        }
        return nullptr;
    }
    GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    pb = oriented;

    const int w = gdk_pixbuf_get_width(pb);
    const int h = gdk_pixbuf_get_height(pb);
    const int nch = gdk_pixbuf_get_n_channels(pb);
    const int rowstride = gdk_pixbuf_get_rowstride(pb);
    const bool has_alpha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *pixels = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_object_unref(pb);
        error = "out of memory";
        return nullptr;
    }
    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; ++y) {
        const guchar *src = pixels + y * rowstride;
        uint32_t *dst = reinterpret_cast<uint32_t *>(data + y * stride);
        for (int x = 0; x < w; ++x, src += nch) {
            const uint32_t a = has_alpha ? src[3] : 255;
            const uint32_t r = (src[0] * a + 127) / 255;
            const uint32_t g = (src[1] * a + 127) / 255;
            const uint32_t b = (src[2] * a + 127) / 255;
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);
    g_object_unref(pb);

    auto res = std::make_shared<Source>();
    res->surface = surf;
    res->width = w;
    res->height = h;
    return res;
}

std::shared_ptr<Source> load_svg(const std::string &fname,
                                 Glib::ustring &error)
{
    GError *err = nullptr;
    RsvgHandle *h = rsvg_handle_new_from_file(fname.c_str(), &err);
    if (!h) {
        error = err ? err->message : "unknown error";
        if (err) {
            g_error_free(err);
        }
        return nullptr;
    }

    double w = 0, h_ = 0;
#if LIBRSVG_CHECK_VERSION(2, 52, 0)
    if (!rsvg_handle_get_intrinsic_size_in_pixels(h, &w, &h_)) {
        gboolean has_w, has_h, has_vb;
        RsvgLength lw, lh;
        RsvgRectangle vb;
        rsvg_handle_get_intrinsic_dimensions(h, &has_w, &lw, &has_h, &lh,
                                             &has_vb, &vb);
        if (has_vb) {
            w = vb.width;
            h_ = vb.height;
        }
    }
#else
    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(h, &dim);
    w = dim.width;
    h_ = dim.height;
#endif

    if (!(w > 0 && h_ > 0)) {
        g_object_unref(h);
        error = "the SVG document has no size";
        return nullptr;
    }

    auto res = std::make_shared<Source>();
    res->svg = h;
    res->width = w;
    res->height = h_;
    return res;
}

cairo_filter_t cairo_filter(WatermarkParams::Interpolation i)
{
    switch (i) {
    case WatermarkParams::Interpolation::NEAREST:
        return CAIRO_FILTER_NEAREST;
    case WatermarkParams::Interpolation::BILINEAR:
        return CAIRO_FILTER_BILINEAR;
    case WatermarkParams::Interpolation::BICUBIC:
        return CAIRO_FILTER_BEST;
    default:
        return CAIRO_FILTER_GOOD;
    }
}

std::shared_ptr<Source> get_source(const WatermarkParams &wp,
                                   Glib::ustring &error)
{
    if (wp.sourceFile().empty()) {
        error = "no file selected";
        return nullptr;
    }

    const std::string fname = Glib::filename_from_utf8(wp.sourceFile());
    const bool is_svg = wp.type == WatermarkParams::Type::SVG;
    const gint64 mtime = file_mtime(fname);
    if (mtime < 0) {
        error = "file not found";
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(cache_mutex);

    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->fname == fname && it->is_svg == is_svg) {
            if (it->mtime == mtime) {
                cache.splice(cache.begin(), cache, it);
                return cache.front().source;
            }
            cache.erase(it);
            break;
        }
    }

    auto src = is_svg ? load_svg(fname, error) : load_image(fname, error);
    if (src) {
        cache.push_front(CacheEntry{fname, is_svg, mtime, src});
        if (cache.size() > CACHE_SIZE) {
            cache.pop_back();
        }
    }
    return src;
}

PangoLayout *create_text_layout(cairo_t *cr, const WatermarkParams &wp,
                                double size_px)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);

    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
    cairo_font_options_destroy(fo);
    pango_layout_context_changed(layout);

    PangoFontDescription *fd =
        pango_font_description_from_string(wp.font.c_str());
    pango_font_description_set_absolute_size(fd, size_px * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    const int col = int(wp.anchor) % 3;
    pango_layout_set_alignment(layout, col == 0   ? PANGO_ALIGN_LEFT
                                       : col == 1 ? PANGO_ALIGN_CENTER
                                                  : PANGO_ALIGN_RIGHT);
    pango_layout_set_text(layout, wp.text.c_str(), -1);
    return layout;
}

struct Content {
    PangoLayout *layout = nullptr;
    std::shared_ptr<Source> src;
    double cw = 0, ch = 0;
    double text_dx = 0, text_dy = 0;

    Content() = default;
    Content(const Content &) = delete;
    Content &operator=(const Content &) = delete;
    ~Content()
    {
        if (layout) {
            g_object_unref(layout);
        }
    }
};

struct Sizes {
    double size, pos_x, pos_y;
};

Sizes get_sizes(const WatermarkParams &wp, double ref_w, double ref_h)
{
    const double base = watermarkPercentBase(ref_w, ref_h);
    const bool percent = wp.unit == WatermarkParams::Unit::PERCENT;
    const auto conv = [&](int px, double pct) -> double {
        return percent ? base * pct / 100.0 : double(px);
    };
    return Sizes{conv(wp.size, wp.sizePercent), conv(wp.posX, wp.posXPercent),
                 conv(wp.posY, wp.posYPercent)};
}

bool get_content(cairo_t *cr, const WatermarkParams &wp, double size,
                 Content &c, Glib::ustring *error)
{
    if (!(size > 0)) {
        return true;
    }
    if (wp.type == WatermarkParams::Type::TEXT) {
        if (wp.text.empty()) {
            return true;
        }
        c.layout = create_text_layout(cr, wp, size);
        PangoRectangle ink, logical;
        pango_layout_get_extents(c.layout, &ink, &logical);
        c.cw = double(logical.width) / PANGO_SCALE;
        c.ch = double(logical.height) / PANGO_SCALE;
        c.text_dx = -double(logical.x) / PANGO_SCALE;
        c.text_dy = -double(logical.y) / PANGO_SCALE;
    } else {
        Glib::ustring err;
        c.src = get_source(wp, err);
        if (!c.src) {
            if (error) {
                *error = wp.sourceFile() + ": " + err;
            }
            return false;
        }
        c.cw = size;
        c.ch = size * c.src->height / c.src->width;
    }
    return true;
}

void place(const WatermarkParams &wp, double ref_w, double ref_h,
           const Content &c, const Sizes &s, WatermarkBox &box)
{
    const double angle = wp.rotation * M_PI / 180.0;
    const double ca = std::abs(std::cos(angle));
    const double sa = std::abs(std::sin(angle));
    box.w = c.cw * ca + c.ch * sa;
    box.h = c.cw * sa + c.ch * ca;

    watermarkAnchorPoint(wp.anchor, ref_w, ref_h, box.anchor_x, box.anchor_y);
    box.point_x = box.anchor_x + s.pos_x;
    box.point_y = box.anchor_y + s.pos_y;
    const int col = int(wp.anchor) % 3;
    const int row = int(wp.anchor) / 3;
    box.cx = box.point_x + (1 - col) * box.w / 2;
    box.cy = box.point_y + (1 - row) * box.h / 2;
}

}

void watermarkAnchorPoint(WatermarkParams::Anchor a, double ref_w,
                          double ref_h, double &x, double &y)
{
    x = (int(a) % 3) * ref_w / 2;
    y = (int(a) / 3) * ref_h / 2;
}

void watermarkPositionFor(WatermarkParams::Anchor a, double ref_w,
                          double ref_h, double bw, double bh, double cx,
                          double cy, double &pos_x, double &pos_y)
{
    double ax, ay;
    watermarkAnchorPoint(a, ref_w, ref_h, ax, ay);
    const int col = int(a) % 3;
    const int row = int(a) / 3;
    pos_x = cx - (1 - col) * bw / 2 - ax;
    pos_y = cy - (1 - row) * bh / 2 - ay;
}

bool checkWatermarkSource(const WatermarkParams &wp, Glib::ustring &error)
{
    if (wp.type == WatermarkParams::Type::TEXT) {
        return true;
    }
    return bool(get_source(wp, error));
}

bool getWatermarkBox(const WatermarkParams &wp, double ref_w, double ref_h,
                     WatermarkBox &box)
{
    if (!(ref_w > 0 && ref_h > 0)) {
        return false;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surf);
    const Sizes s = get_sizes(wp, ref_w, ref_h);
    bool ok;
    {
        Content c;
        ok = get_content(cr, wp, s.size, c, nullptr) && c.cw > 0 && c.ch > 0;
        if (ok) {
            place(wp, ref_w, ref_h, c, s, box);
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return ok;
}

bool getWatermarkFrame(const procparams::CropParams &crop,
                       const procparams::BorderParams &border,
                       const WatermarkParams &wp, int full_w, int full_h,
                       double out_scale, WatermarkFrame &f)
{
    if (crop.enabled) {
        f.image_x = crop.x;
        f.image_y = crop.y;
        f.image_w = crop.w;
        f.image_h = crop.h;
    } else {
        f.image_x = f.image_y = 0;
        f.image_w = full_w;
        f.image_h = full_h;
    }
    if (!(f.image_w > 0 && f.image_h > 0)) {
        return false;
    }

    f.canvas_x = f.image_x;
    f.canvas_y = f.image_y;
    f.canvas_w = f.image_w;
    f.canvas_h = f.image_h;
    if (border.enabled &&
        border.placement == procparams::BorderParams::Placement::OUTSIDE) {
        double side, bottom;
        border.getWidths(f.image_w, f.image_h, out_scale, side, bottom);
        f.canvas_x -= side;
        f.canvas_y -= side;
        f.canvas_w += 2 * side;
        f.canvas_h += side + bottom;
    }

    const bool canvas = wp.reference == WatermarkParams::Reference::CANVAS;
    f.ref_x = canvas ? f.canvas_x : f.image_x;
    f.ref_y = canvas ? f.canvas_y : f.image_y;
    f.ref_w = canvas ? f.canvas_w : f.image_w;
    f.ref_h = canvas ? f.canvas_h : f.image_h;
    return true;
}

bool drawWatermark(cairo_t *cr, const WatermarkParams &wp, double ref_w,
                   double ref_h, Glib::ustring *error)
{
    if (!(ref_w > 0 && ref_h > 0) || !(wp.opacity > 0)) {
        return true;
    }

    const Sizes s = get_sizes(wp, ref_w, ref_h);
    Content c;
    if (!get_content(cr, wp, s.size, c, error)) {
        return false;
    }
    if (!(c.cw > 0 && c.ch > 0)) {
        return true;
    }

    WatermarkBox box;
    place(wp, ref_w, ref_h, c, s, box);

    cairo_save(cr);
    cairo_push_group(cr);
    cairo_translate(cr, box.cx, box.cy);
    cairo_rotate(cr, -wp.rotation * M_PI / 180.0);
    cairo_translate(cr, -c.cw / 2, -c.ch / 2);

    if (c.layout) {
        cairo_set_source_rgb(cr, wp.red, wp.green, wp.blue);
        cairo_move_to(cr, c.text_dx, c.text_dy);
        pango_cairo_update_layout(cr, c.layout);
        pango_cairo_show_layout(cr, c.layout);
    } else if (c.src->surface) {
        cairo_scale(cr, c.cw / c.src->width, c.ch / c.src->height);
        cairo_set_source_surface(cr, c.src->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr),
                                 cairo_filter(wp.interpolation));
        cairo_rectangle(cr, 0, 0, c.src->width, c.src->height);
        cairo_fill(cr);
    } else if (c.src->svg) {
        std::lock_guard<std::mutex> lock(svg_render_mutex);
#if LIBRSVG_CHECK_VERSION(2, 52, 0)
        RsvgRectangle viewport = {0, 0, c.cw, c.ch};
        rsvg_handle_render_document(c.src->svg, cr, &viewport, nullptr);
#else
        cairo_scale(cr, c.cw / c.src->width, c.ch / c.src->height);
        rsvg_handle_render_cairo(c.src->svg, cr);
#endif
    }

    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, std::min(wp.opacity, 100.0) / 100.0);
    cairo_restore(cr);

    return true;
}

}}
