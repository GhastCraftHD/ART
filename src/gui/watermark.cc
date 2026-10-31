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
#include "../engine/watermark.h"
#include "watermark.h"
#include "../engine/improcfun.h"
#include "eventmapper.h"
#include "options.h"
#include "paramsedited.h"
#include "rtimage.h"
#include "shortcutsmanager.h"
#include <cmath>
#include <cstdio>

namespace art { namespace gui {
using namespace utils;

using namespace art::engine;
using namespace art::engine::procparams;

namespace {

constexpr int TEXT_DELAY_MS = 600;

constexpr double SNAP_DISTANCE = 0.015;

enum {
    GEOM_BOX = 0,
    GEOM_ANCHOR0 = 1,
    GEOM_LINK = 10,
    GEOM_SNAP_V = 11,
    GEOM_SNAP_H = 12
};

Glib::ustring color_to_string(const Gdk::RGBA &c)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             int(std::round(c.get_red() * 255)),
             int(std::round(c.get_green() * 255)),
             int(std::round(c.get_blue() * 255)));
    return buf;
}

Glib::ustring font_without_size(const Glib::ustring &name)
{
    Pango::FontDescription fd(name);
    fd.unset_fields(Pango::FONT_MASK_SIZE);
    return fd.to_string();
}

Gtk::HBox *labeled(const Glib::ustring &label, Gtk::Widget &w)
{
    Gtk::HBox *hb = Gtk::manage(new Gtk::HBox());
    hb->pack_start(*Gtk::manage(new Gtk::Label(label + ": ")),
                   Gtk::PACK_SHRINK);
    hb->pack_start(w);
    return hb;
}

}

Watermark::Watermark()
    : FoldableToolPanel(this, "watermark", M("TP_WATERMARK_LABEL"), false,
                        true, true),
      EditSubscriber(ET_OBJECTS), lastObject(-1), dragging(false),
      dragStartX(0), dragStartY(0), snapCol(-1), snapRow(-1)
{
    auto m = ProcEventMapper::getInstance();
    const auto ev = [&](const char *key) { return m->newEvent(CROP, key); };
    EvEnabled = ev("HISTORY_MSG_WATERMARK_ENABLED");
    EvType = ev("HISTORY_MSG_WATERMARK_TYPE");
    EvText = ev("HISTORY_MSG_WATERMARK_TEXT");
    EvFont = ev("HISTORY_MSG_WATERMARK_FONT");
    EvColor = ev("HISTORY_MSG_WATERMARK_COLOR");
    EvFile = ev("HISTORY_MSG_WATERMARK_FILE");
    EvInterpolation = ev("HISTORY_MSG_WATERMARK_INTERPOLATION");
    EvUnit = ev("HISTORY_MSG_WATERMARK_UNIT");
    EvSize = ev("HISTORY_MSG_WATERMARK_SIZE");
    EvPosition = ev("HISTORY_MSG_WATERMARK_POSITION");
    EvAnchor = ev("HISTORY_MSG_WATERMARK_ANCHOR");
    EvReference = ev("HISTORY_MSG_WATERMARK_REFERENCE");
    EvOpacity = ev("HISTORY_MSG_WATERMARK_OPACITY");
    EvRotation = ev("HISTORY_MSG_WATERMARK_ROTATION");
    EvDefaults = ev("HISTORY_MSG_WATERMARK_DEFAULTS");
    EvToolReset.set_action(CROP);

    type = Gtk::manage(new MyComboBoxText());
    type->append(M("TP_WATERMARK_TYPE_TEXT"));
    type->append(M("TP_WATERMARK_TYPE_IMAGE"));
    type->append(M("TP_WATERMARK_TYPE_SVG"));
    type->set_active(0);
    type->signal_changed().connect(sigc::mem_fun(*this, &Watermark::typeChanged));

    text = Gtk::manage(new Gtk::Entry());
    text->signal_changed().connect(sigc::mem_fun(*this, &Watermark::textChanged));
    text->signal_activate().connect([this]() {
        textDelay.disconnect();
        textChangedDelayed();
    });

    font = Gtk::manage(new Gtk::FontButton());
    font->set_show_size(false);
    font->set_use_font(true);
    font->set_title(M("TP_WATERMARK_FONT"));
    font->signal_font_set().connect(sigc::mem_fun(*this, &Watermark::fontChanged));

    color = Gtk::manage(new Gtk::ColorButton());
    color->set_use_alpha(false);
    color->set_title(M("TP_WATERMARK_COLOR"));
    color->signal_color_set().connect(
        sigc::mem_fun(*this, &Watermark::colorChanged));

    textBox = Gtk::manage(new Gtk::VBox());
    textBox->pack_start(*labeled(M("TP_WATERMARK_TEXT"), *text));
    textBox->pack_start(*labeled(M("TP_WATERMARK_FONT"), *font));
    textBox->pack_start(*labeled(M("TP_WATERMARK_COLOR"), *color));
    textBox->show_all();
    textBox->set_no_show_all(true);

    file = Gtk::manage(new MyFileChooserButton(M("TP_WATERMARK_FILE")));
    imageFilter = Gtk::FileFilter::create();
    imageFilter->set_name(M("TP_WATERMARK_FILTER_IMAGES"));
    imageFilter->add_pixbuf_formats();
    svgFilter = Gtk::FileFilter::create();
    svgFilter->set_name(M("TP_WATERMARK_FILTER_SVG"));
    svgFilter->add_pattern("*.svg");
    svgFilter->add_pattern("*.SVG");
    svgFilter->add_mime_type("image/svg+xml");
    file->signal_selection_changed().connect(
        sigc::mem_fun(*this, &Watermark::fileChanged));

    fileWarning = Gtk::manage(new Gtk::HBox());
    fileWarning->pack_start(*Gtk::manage(new RTImage("warning-small.svg")),
                            Gtk::PACK_SHRINK, 2);
    fileWarningLabel = Gtk::manage(new Gtk::Label());
    fileWarningLabel->set_line_wrap(true);
    fileWarningLabel->set_xalign(0);
    fileWarning->pack_start(*fileWarningLabel);
    fileWarning->show_all();
    fileWarning->set_no_show_all(true);

    interpolation = Gtk::manage(new MyComboBoxText());
    interpolation->append(M("TP_WATERMARK_INTERPOLATION_NEAREST"));
    interpolation->append(M("TP_WATERMARK_INTERPOLATION_BILINEAR"));
    interpolation->append(M("TP_WATERMARK_INTERPOLATION_GOOD"));
    interpolation->append(M("TP_WATERMARK_INTERPOLATION_BICUBIC"));
    interpolation->set_active(int(WatermarkParams::Interpolation::GOOD));
    interpolation->set_tooltip_markup(M("TP_WATERMARK_INTERPOLATION_TOOLTIP"));
    interpolation->signal_changed().connect(
        sigc::mem_fun(*this, &Watermark::interpolationChanged));
    interpolationBox =
        labeled(M("TP_WATERMARK_INTERPOLATION"), *interpolation);
    interpolationBox->show_all();
    interpolationBox->set_no_show_all(true);

    fileBox = Gtk::manage(new Gtk::VBox());
    fileBox->pack_start(*labeled(M("TP_WATERMARK_FILE"), *file));
    fileBox->pack_start(*fileWarning);
    fileBox->pack_start(*interpolationBox);
    fileBox->show_all();
    fileBox->set_no_show_all(true);

    unit = Gtk::manage(new MyComboBoxText());
    unit->append(M("TP_WATERMARK_UNIT_PIXELS"));
    unit->append(M("TP_WATERMARK_UNIT_PERCENT"));
    unit->set_active(1);
    unit->set_tooltip_text(M("TP_WATERMARK_UNIT_TOOLTIP"));
    unit->signal_changed().connect(sigc::mem_fun(*this, &Watermark::unitChanged));

    const auto adj = [this](const char *label, double lo, double hi,
                            double step, double def,
                            const char *tooltip) -> Adjuster * {
        Adjuster *a = Gtk::manage(new Adjuster(M(label), lo, hi, step, def));
        if (tooltip) {
            a->set_tooltip_text(M(tooltip));
        }
        a->setAdjusterListener(this);
        a->show();
        return a;
    };
    size = adj("TP_WATERMARK_SIZE_PX", 1, 2000, 1, 60, "TP_WATERMARK_SIZE_TOOLTIP");
    posX = adj("TP_WATERMARK_POSX_PX", -4000, 4000, 1, -30, "TP_WATERMARK_POS_TOOLTIP");
    posY = adj("TP_WATERMARK_POSY_PX", -4000, 4000, 1, -30, "TP_WATERMARK_POS_TOOLTIP");
    sizePercent = adj("TP_WATERMARK_SIZE_PERCENT", 0.1, 100, 0.1, 4, "TP_WATERMARK_SIZE_TOOLTIP");
    posXPercent = adj("TP_WATERMARK_POSX_PERCENT", -100, 100, 0.1, -2, "TP_WATERMARK_POS_TOOLTIP");
    posYPercent = adj("TP_WATERMARK_POSY_PERCENT", -100, 100, 0.1, -2, "TP_WATERMARK_POS_TOOLTIP");

    pixelBox = Gtk::manage(new Gtk::VBox());
    pixelBox->pack_start(*size);
    pixelBox->pack_start(*posX);
    pixelBox->pack_start(*posY);
    pixelBox->set_no_show_all(true);

    percentBox = Gtk::manage(new Gtk::VBox());
    percentBox->pack_start(*sizePercent);
    percentBox->pack_start(*posXPercent);
    percentBox->pack_start(*posYPercent);
    percentBox->set_no_show_all(true);

    anchor = Gtk::manage(new MyComboBoxText());
    for (const char *k :
         {"TP_WATERMARK_ANCHOR_TOPLEFT", "TP_WATERMARK_ANCHOR_TOP",
          "TP_WATERMARK_ANCHOR_TOPRIGHT", "TP_WATERMARK_ANCHOR_LEFT",
          "TP_WATERMARK_ANCHOR_CENTER", "TP_WATERMARK_ANCHOR_RIGHT",
          "TP_WATERMARK_ANCHOR_BOTTOMLEFT", "TP_WATERMARK_ANCHOR_BOTTOM",
          "TP_WATERMARK_ANCHOR_BOTTOMRIGHT"}) {
        anchor->append(M(k));
    }
    anchor->set_active(int(WatermarkParams::Anchor::BOTTOM_RIGHT));
    anchor->set_tooltip_text(M("TP_WATERMARK_ANCHOR_TOOLTIP"));
    anchor->signal_changed().connect(
        sigc::mem_fun(*this, &Watermark::anchorChanged));

    edit = Gtk::manage(new Gtk::ToggleButton());
    edit->get_style_context()->add_class("independent");
    edit->add(*Gtk::manage(new RTImage("crosshair-adjust.svg")));
    edit->set_tooltip_markup(M("TP_WATERMARK_EDIT_TOOLTIP"));
    editConn = edit->signal_toggled().connect(
        sigc::mem_fun(*this, &Watermark::editToggled));

    Gtk::HBox *hb_anchor = labeled(M("TP_WATERMARK_ANCHOR"), *anchor);
    hb_anchor->pack_start(*edit, Gtk::PACK_SHRINK, 2);

    reference = Gtk::manage(new MyComboBoxText());
    reference->append(M("TP_WATERMARK_REFERENCE_IMAGE"));
    reference->append(M("TP_WATERMARK_REFERENCE_CANVAS"));
    reference->set_active(0);
    reference->set_tooltip_text(M("TP_WATERMARK_REFERENCE_TOOLTIP"));
    reference->signal_changed().connect(
        sigc::mem_fun(*this, &Watermark::referenceChanged));

    opacity = adj("TP_WATERMARK_OPACITY", 0, 100, 1, 60, nullptr);
    rotation = adj("TP_WATERMARK_ROTATION", -180, 180, 0.1, 0, nullptr);

    Gtk::Button *save_def =
        Gtk::manage(new Gtk::Button(M("TP_WATERMARK_DEFAULT_SAVE")));
    save_def->set_tooltip_text(M("TP_WATERMARK_DEFAULT_SAVE_TOOLTIP"));
    save_def->signal_clicked().connect(
        sigc::mem_fun(*this, &Watermark::saveDefaultPressed));
    Gtk::Button *load_def =
        Gtk::manage(new Gtk::Button(M("TP_WATERMARK_DEFAULT_LOAD")));
    load_def->set_tooltip_text(M("TP_WATERMARK_DEFAULT_LOAD_TOOLTIP"));
    load_def->signal_clicked().connect(
        sigc::mem_fun(*this, &Watermark::loadDefaultPressed));
    Gtk::HBox *def_box = Gtk::manage(new Gtk::HBox(true, 2));
    def_box->pack_start(*save_def);
    def_box->pack_start(*load_def);

    Gtk::HBox *hb_type = labeled(M("TP_WATERMARK_TYPE"), *type);
    Gtk::HBox *hb_unit = labeled(M("TP_WATERMARK_UNIT"), *unit);
    Gtk::HBox *hb_ref = labeled(M("TP_WATERMARK_REFERENCE"), *reference);
    for (auto w : std::initializer_list<Gtk::Widget *>{hb_type, hb_unit, hb_anchor, hb_ref, def_box}) {
        w->show_all();
    }

    pack_start(*hb_type);
    pack_start(*textBox);
    pack_start(*fileBox);
    pack_start(*hb_unit);
    pack_start(*hb_anchor);
    pack_start(*pixelBox);
    pack_start(*percentBox);
    pack_start(*hb_ref);
    pack_start(*opacity);
    pack_start(*rotation);
    pack_start(*def_box, Gtk::PACK_SHRINK, 4);

    {
        Rectangle *box = new Rectangle();
        box->datum = Geometry::IMAGE;
        box->filled = false;
        box->innerLineWidth = 1.5;
        visibleGeometry.push_back(box);

        for (int i = 0; i < 9; ++i) {
            Circle *c = new Circle();
            c->datum = Geometry::IMAGE;
            c->radiusInImageSpace = false;
            c->radius = 4;
            c->filled = true;
            visibleGeometry.push_back(c);
        }

        for (int i = 0; i < 3; ++i) {
            Line *l = new Line();
            l->datum = Geometry::IMAGE;
            l->innerLineWidth = 1;
            l->setDashed(true);
            visibleGeometry.push_back(l);
        }

        Rectangle *mo_box = new Rectangle();
        mo_box->datum = Geometry::IMAGE;
        mo_box->filled = true;
        mouseOverGeometry.push_back(mo_box);
    }

    readParams(WatermarkParams());
}

Watermark::~Watermark()
{
    textDelay.disconnect();
    for (auto g : visibleGeometry) {
        delete g;
    }
    for (auto g : mouseOverGeometry) {
        delete g;
    }
}

void Watermark::readParams(const WatermarkParams &wp)
{
    imageFile = wp.imageFile;
    svgFile = wp.svgFile;
    type->set_active(int(wp.type));
    text->set_text(wp.text);
    font->set_font_name(wp.font);
    Gdk::RGBA c;
    c.set_rgba(wp.red, wp.green, wp.blue, 1.0);
    color->set_rgba(c);
    updateFileFilter();
    showFile();
    interpolation->set_active(int(wp.interpolation));
    unit->set_active(wp.unit == WatermarkParams::Unit::PERCENT ? 1 : 0);
    size->setValue(wp.size);
    posX->setValue(wp.posX);
    posY->setValue(wp.posY);
    sizePercent->setValue(wp.sizePercent);
    posXPercent->setValue(wp.posXPercent);
    posYPercent->setValue(wp.posYPercent);
    anchor->set_active(int(wp.anchor));
    reference->set_active(int(wp.reference));
    opacity->setValue(wp.opacity);
    rotation->setValue(wp.rotation);

    updateVisibility();
    checkSource();
    updateGeometry();
}

WatermarkParams Watermark::getParams()
{
    WatermarkParams wp;
    wp.enabled = getEnabled();
    wp.type = WatermarkParams::Type(std::max(type->get_active_row_number(), 0));
    wp.text = text->get_text();
    wp.font = font_without_size(font->get_font_name());
    const Gdk::RGBA c = color->get_rgba();
    wp.red = c.get_red();
    wp.green = c.get_green();
    wp.blue = c.get_blue();
    wp.imageFile = imageFile;
    wp.svgFile = svgFile;
    wp.interpolation = WatermarkParams::Interpolation(
        std::max(interpolation->get_active_row_number(), 0));
    wp.unit = unit->get_active_row_number() == 1
                  ? WatermarkParams::Unit::PERCENT
                  : WatermarkParams::Unit::PIXELS;
    wp.size = size->getValue();
    wp.posX = posX->getValue();
    wp.posY = posY->getValue();
    wp.sizePercent = sizePercent->getValue();
    wp.posXPercent = posXPercent->getValue();
    wp.posYPercent = posYPercent->getValue();
    wp.anchor =
        WatermarkParams::Anchor(std::max(anchor->get_active_row_number(), 0));
    wp.reference = WatermarkParams::Reference(
        std::max(reference->get_active_row_number(), 0));
    wp.opacity = opacity->getValue();
    wp.rotation = rotation->getValue();
    return wp;
}

void Watermark::read(const ProcParams *pp)
{
    disableListener();
    textDelay.disconnect();
    setEnabled(pp->watermark.enabled);
    updateContext(*pp);
    readParams(pp->watermark);
    enableListener();
}

void Watermark::write(ProcParams *pp) { pp->watermark = getParams(); }

void Watermark::setDefaults(const ProcParams *defParams)
{
    const auto &d = defParams->watermark;
    size->setDefault(d.size);
    posX->setDefault(d.posX);
    posY->setDefault(d.posY);
    sizePercent->setDefault(d.sizePercent);
    posXPercent->setDefault(d.posXPercent);
    posYPercent->setDefault(d.posYPercent);
    opacity->setDefault(d.opacity);
    rotation->setDefault(d.rotation);

    initial_params = d;
}

void Watermark::updateContext(const ProcParams &pp)
{
    context.crop = pp.crop;
    context.border = pp.border;
    context.resize = pp.resize;
    updateGeometry();
}

void Watermark::updateVisibility()
{
    const bool is_text = type->get_active_row_number() == 0;
    textBox->set_visible(is_text);
    fileBox->set_visible(!is_text);
    interpolationBox->set_visible(type->get_active_row_number() == 1);

    const bool percent = unit->get_active_row_number() == 1;
    pixelBox->set_visible(!percent);
    percentBox->set_visible(percent);

    size->setLabel(M(is_text ? "TP_WATERMARK_TEXTSIZE_PX" : "TP_WATERMARK_SIZE_PX"));
    sizePercent->setLabel(
        M(is_text ? "TP_WATERMARK_TEXTSIZE_PERCENT" : "TP_WATERMARK_SIZE_PERCENT"));
}

void Watermark::updateFileFilter()
{
    const bool svg = type->get_active_row_number() == 2;
    for (auto &f : file->list_filters()) {
        file->remove_filter(f);
    }
    file->add_filter(svg ? svgFilter : imageFilter);
    file->set_filter(svg ? svgFilter : imageFilter);
}

void Watermark::showFile()
{
    const int t = type->get_active_row_number();
    const Glib::ustring &fn = t == 1 ? imageFile : t == 2 ? svgFile : "";
    if (fn.empty()) {
        file->unselect_all();
    } else {
        file->set_filename(Glib::filename_from_utf8(fn));
    }
}

void Watermark::checkSource()
{
    const WatermarkParams wp = getParams();
    Glib::ustring err;
    if (wp.type == WatermarkParams::Type::TEXT ||
        art::engine::checkWatermarkSource(wp, err)) {
        fileWarning->hide();
    } else {
        fileWarningLabel->set_text(M("TP_WATERMARK_FILE_ERROR") + ": " + err);
        fileWarning->show();
    }
}

bool Watermark::getPlacement(WatermarkFrame &frame, double &out_scale,
                             WatermarkBox &box)
{
    EditDataProvider *provider = getEditProvider();
    if (!provider) {
        return false;
    }
    int fw = 0, fh = 0;
    provider->getImageSize(fw, fh);
    if (fw <= 0 || fh <= 0) {
        return false;
    }
    const WatermarkParams wp = getParams();
    out_scale = ImProcFunctions::outputScale(&context, fw, fh);
    return getWatermarkFrame(context.crop, context.border, wp, fw, fh,
                             out_scale, frame) &&
           getWatermarkBox(wp, frame.ref_w * out_scale,
                           frame.ref_h * out_scale, box);
}

void Watermark::updateGeometry()
{
    WatermarkFrame f;
    WatermarkBox box;
    double out_scale = 1;
    const bool ok = getEnabled() && getPlacement(f, out_scale, box);

    for (auto g : visibleGeometry) {
        g->setVisible(ok);
    }
    mouseOverGeometry[GEOM_BOX]->setVisible(ok);
    if (!ok) {
        return;
    }

    const auto ix = [&](double v) { return int(std::round(f.ref_x + v / out_scale)); };
    const auto iy = [&](double v) { return int(std::round(f.ref_y + v / out_scale)); };

    const double rw = f.ref_w * out_scale;
    const double rh = f.ref_h * out_scale;

    for (auto r : {static_cast<Rectangle *>(visibleGeometry[GEOM_BOX]),
                   static_cast<Rectangle *>(mouseOverGeometry[GEOM_BOX])}) {
        r->setXYXY(ix(box.cx - box.w / 2), iy(box.cy - box.h / 2),
                   ix(box.cx + box.w / 2), iy(box.cy + box.h / 2));
    }

    const int active = anchor->get_active_row_number();
    for (int i = 0; i < 9; ++i) {
        Circle *c = static_cast<Circle *>(visibleGeometry[GEOM_ANCHOR0 + i]);
        double ax, ay;
        watermarkAnchorPoint(WatermarkParams::Anchor(i), rw, rh, ax, ay);
        c->center.set(ix(ax), iy(ay));
        c->radius = i == active ? 6 : 4;
        c->state = i == active ? Geometry::ACTIVE : Geometry::NORMAL;
    }

    Line *link = static_cast<Line *>(visibleGeometry[GEOM_LINK]);
    link->begin.set(ix(box.anchor_x), iy(box.anchor_y));
    link->end.set(ix(box.point_x), iy(box.point_y));

    Line *sv = static_cast<Line *>(visibleGeometry[GEOM_SNAP_V]);
    Line *sh = static_cast<Line *>(visibleGeometry[GEOM_SNAP_H]);
    sv->setVisible(dragging && snapCol >= 0);
    sh->setVisible(dragging && snapRow >= 0);
    if (snapCol >= 0) {
        const double x = snapCol * rw / 2;
        sv->begin.set(ix(x), iy(0));
        sv->end.set(ix(x), iy(rh));
    }
    if (snapRow >= 0) {
        const double y = snapRow * rh / 2;
        sh->begin.set(ix(0), iy(y));
        sh->end.set(ix(rw), iy(y));
    }
}

void Watermark::setEditProvider(EditDataProvider *provider)
{
    EditSubscriber::setEditProvider(provider);
    updateGeometry();
}

void Watermark::editToggled()
{
    if (edit->get_active()) {
        updateGeometry();
        subscribe();
    } else {
        unsubscribe();
    }
}

void Watermark::switchOffEditMode()
{
    if (edit->get_active()) {
        const bool was_blocked = editConn.block(true);
        edit->set_active(false);
        if (!was_blocked) {
            editConn.block(false);
        }
    }
    EditSubscriber::switchOffEditMode();
}

CursorShape Watermark::getCursor(int objectID)
{
    return objectID == GEOM_BOX ? CSMove2D : CSArrow;
}

bool Watermark::mouseOver(int modifierKey)
{
    EditDataProvider *provider = getEditProvider();
    if (!provider || provider->object == lastObject) {
        return false;
    }
    visibleGeometry[GEOM_BOX]->state =
        provider->object == GEOM_BOX ? Geometry::PRELIGHT : Geometry::NORMAL;
    lastObject = provider->object;
    return true;
}

bool Watermark::button1Pressed(int modifierKey)
{
    if (lastObject != GEOM_BOX ||
        (modifierKey & ShortcutsManager::mousePrimaryMask())) {
        return false;
    }
    WatermarkFrame f;
    WatermarkBox box;
    double out_scale;
    if (!getPlacement(f, out_scale, box)) {
        return false;
    }
    dragStartX = box.cx;
    dragStartY = box.cy;
    dragging = true;
    snapCol = snapRow = -1;
    visibleGeometry[GEOM_BOX]->state = Geometry::DRAGGED;
    EditSubscriber::action = ES_ACTION_DRAGGING;
    return true;
}

bool Watermark::button1Released()
{
    dragging = false;
    snapCol = snapRow = -1;
    visibleGeometry[GEOM_BOX]->state =
        lastObject == GEOM_BOX ? Geometry::PRELIGHT : Geometry::NORMAL;
    EditSubscriber::action = ES_ACTION_NONE;
    updateGeometry();
    return true;
}

bool Watermark::drag1(int modifierKey)
{
    EditDataProvider *provider = getEditProvider();
    WatermarkFrame f;
    WatermarkBox box;
    double out_scale;
    if (!dragging || !provider || !getPlacement(f, out_scale, box)) {
        return false;
    }

    const double rw = f.ref_w * out_scale;
    const double rh = f.ref_h * out_scale;
    double cx = dragStartX + provider->deltaImage.x * out_scale;
    double cy = dragStartY + provider->deltaImage.y * out_scale;

    int col = std::max(0, std::min(2, int(std::floor(cx / (rw / 3)))));
    int row = std::max(0, std::min(2, int(std::floor(cy / (rh / 3)))));

    snapCol = snapRow = -1;
    if (!(modifierKey & GDK_SHIFT_MASK)) {
        const double thr = SNAP_DISTANCE * watermarkPercentBase(rw, rh);
        double best_x = thr, best_y = thr;
        for (int i = 0; i < 3; ++i) {
            const double dx = std::abs((cx - box.w / 2 + i * box.w / 2) - i * rw / 2);
            if (dx < best_x) {
                best_x = dx;
                snapCol = i;
            }
            const double dy = std::abs((cy - box.h / 2 + i * box.h / 2) - i * rh / 2);
            if (dy < best_y) {
                best_y = dy;
                snapRow = i;
            }
        }
        if (snapCol >= 0) {
            col = snapCol;
            cx = snapCol * rw / 2 + box.w / 2 - snapCol * box.w / 2;
        }
        if (snapRow >= 0) {
            row = snapRow;
            cy = snapRow * rh / 2 + box.h / 2 - snapRow * box.h / 2;
        }
    }

    const auto a = WatermarkParams::Anchor(row * 3 + col);
    double px, py;
    watermarkPositionFor(a, rw, rh, box.w, box.h, cx, cy, px, py);

    disableListener();
    anchor->set_active(int(a));
    if (unit->get_active_row_number() == 1) {
        const double base = watermarkPercentBase(rw, rh);
        posXPercent->setValue(px / base * 100.0);
        posYPercent->setValue(py / base * 100.0);
    } else {
        posX->setValue(std::round(px));
        posY->setValue(std::round(py));
    }
    enableListener();

    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(
            EvPosition,
            Glib::ustring::compose("%1: %2, %3", anchor->get_active_text(),
                                   (unit->get_active_row_number() == 1
                                        ? posXPercent
                                        : posX)->getTextValue(),
                                   (unit->get_active_row_number() == 1
                                        ? posYPercent
                                        : posY)->getTextValue()));
    }
    return true;
}

Glib::ustring Watermark::defaultsFile()
{
    return Glib::build_filename(Options::user_config_dir,
                                "watermark_default.arp");
}

bool Watermark::loadUserDefaults(WatermarkParams &wp)
{
    const Glib::ustring fname = defaultsFile();
    if (!Glib::file_test(fname, Glib::FILE_TEST_EXISTS)) {
        return false;
    }
    ProcParams pp;
    ParamsEdited pe(false);
    pe.watermark = true;
    if (pp.load(nullptr, fname, &pe) != 0) {
        return false;
    }
    wp = pp.watermark;
    return true;
}

void Watermark::saveDefaultPressed()
{
    ProcParams pp;
    pp.watermark = getParams();
    pp.watermark.enabled = false;
    ParamsEdited pe(false);
    pe.watermark = true;
    const Glib::ustring fname = defaultsFile();
    if (pp.save(nullptr, fname, "", &pe) != 0) {
        Gtk::MessageDialog msg(*static_cast<Gtk::Window *>(get_toplevel()),
                               M("TP_WATERMARK_DEFAULT_SAVE_ERROR") + "\n" +
                                   fname,
                               false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msg.run();
    }
}

void Watermark::loadDefaultPressed()
{
    WatermarkParams wp;
    if (!loadUserDefaults(wp)) {
        Gtk::MessageDialog msg(*static_cast<Gtk::Window *>(get_toplevel()),
                               M("TP_WATERMARK_DEFAULT_NONE"), false,
                               Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
        msg.run();
        return;
    }
    disableListener();
    readParams(wp);
    enableListener();
    if (listener && getEnabled()) {
        listener->panelChanged(EvDefaults, M("TP_WATERMARK_DEFAULT_LOAD"));
    }
}

void Watermark::adjusterChanged(Adjuster *a, double newval)
{
    updateGeometry();
    if (!listener || !getEnabled()) {
        return;
    }
    if (a == size || a == sizePercent) {
        listener->panelChanged(EvSize, a->getTextValue());
    } else if (a == posX || a == posY || a == posXPercent ||
               a == posYPercent) {
        listener->panelChanged(EvPosition, a->getTextValue());
    } else if (a == opacity) {
        listener->panelChanged(EvOpacity, a->getTextValue());
    } else if (a == rotation) {
        listener->panelChanged(EvRotation, a->getTextValue());
    }
}

void Watermark::typeChanged()
{
    updateVisibility();
    updateFileFilter();
    showFile();
    checkSource();
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(EvType, type->get_active_text());
    }
}

void Watermark::textChanged()
{
    textDelay.disconnect();
    textDelay = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &Watermark::textChangedDelayed), TEXT_DELAY_MS);
}

bool Watermark::textChangedDelayed()
{
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(EvText, text->get_text());
    }
    return false;
}

void Watermark::fontChanged()
{
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(EvFont, font_without_size(font->get_font_name()));
    }
}

void Watermark::colorChanged()
{
    if (listener && getEnabled()) {
        listener->panelChanged(EvColor, color_to_string(color->get_rgba()));
    }
}

void Watermark::fileChanged()
{
    const std::string fn = file->get_filename();
    const Glib::ustring ufn =
        fn.empty() ? Glib::ustring() : Glib::filename_to_utf8(fn);
    switch (type->get_active_row_number()) {
    case 1:
        imageFile = ufn;
        break;
    case 2:
        svgFile = ufn;
        break;
    default:
        break;
    }
    checkSource();
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(
            EvFile, Glib::path_get_basename(file->get_filename()));
    }
}

void Watermark::interpolationChanged()
{
    if (listener && getEnabled()) {
        listener->panelChanged(EvInterpolation,
                               interpolation->get_active_text());
    }
}

void Watermark::unitChanged()
{
    updateVisibility();
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(EvUnit, unit->get_active_text());
    }
}

void Watermark::anchorChanged()
{
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(EvAnchor, anchor->get_active_text());
    }
}

void Watermark::referenceChanged()
{
    updateGeometry();
    if (listener && getEnabled()) {
        listener->panelChanged(EvReference, reference->get_active_text());
    }
}

void Watermark::enabledChanged()
{
    if (!listener) {
        updateGeometry();
        return;
    }
    if (get_inconsistent()) {
        listener->panelChanged(EvEnabled, M("GENERAL_UNCHANGED"));
        return;
    }
    if (getEnabled()) {
        WatermarkParams cur = getParams();
        cur.enabled = false;
        WatermarkParams wp;
        if (cur == WatermarkParams() && loadUserDefaults(wp)) {
            disableListener();
            readParams(wp);
            enableListener();
        }
        updateGeometry();
        listener->panelChanged(EvEnabled, M("GENERAL_ENABLED"));
    } else {
        updateGeometry();
        listener->panelChanged(EvEnabled, M("GENERAL_DISABLED"));
    }
}

void Watermark::toolReset(bool to_initial)
{
    WatermarkParams wp;
    if (to_initial) {
        wp = initial_params;
    } else {
        loadUserDefaults(wp);
    }
    disableListener();
    readParams(wp);
    enableListener();
}

}}
