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
#include "border.h"
#include "eventmapper.h"
#include <cmath>
#include <cstdio>

namespace art { namespace gui {
using namespace utils;

using namespace art::engine;
using namespace art::engine::procparams;

namespace {

Glib::ustring color_to_string(const Gdk::RGBA &c)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             int(std::round(c.get_red() * 255)),
             int(std::round(c.get_green() * 255)),
             int(std::round(c.get_blue() * 255)));
    return buf;
}

} // namespace

Border::Border()
    : FoldableToolPanel(this, "border", M("TP_BORDER_LABEL"), false, true, true)
{
    // The border is not computed in the preview pipeline, but drawn as an
    // overlay by the image view. CROP only refreshes the displayed crops
    // (which carry the border parameters to the view) without reprocessing.
    auto m = ProcEventMapper::getInstance();
    EvEnabled = m->newEvent(art::engine::CROP, "HISTORY_MSG_BORDER_ENABLED");
    EvPlacement =
        m->newEvent(art::engine::CROP, "HISTORY_MSG_BORDER_PLACEMENT");
    EvUnit = m->newEvent(art::engine::CROP, "HISTORY_MSG_BORDER_UNIT");
    EvSize = m->newEvent(art::engine::CROP, "HISTORY_MSG_BORDER_SIZE");
    EvBottomExtra =
        m->newEvent(art::engine::CROP, "HISTORY_MSG_BORDER_BOTTOMEXTRA");
    EvColor = m->newEvent(art::engine::CROP, "HISTORY_MSG_BORDER_COLOR");
    EvToolReset.set_action(art::engine::CROP);

    Gtk::HBox *hb = Gtk::manage(new Gtk::HBox());
    hb->pack_start(
        *Gtk::manage(new Gtk::Label(M("TP_BORDER_PLACEMENT") + ": ")),
        Gtk::PACK_SHRINK);
    placement = Gtk::manage(new MyComboBoxText());
    placement->append(M("TP_BORDER_PLACEMENT_OUTSIDE"));
    placement->append(M("TP_BORDER_PLACEMENT_INSIDE"));
    placement->set_tooltip_markup(M("TP_BORDER_PLACEMENT_TOOLTIP"));
    placement->signal_changed().connect(
        sigc::mem_fun(*this, &Border::placementChanged));
    hb->pack_start(*placement);
    hb->show_all();

    Gtk::HBox *hbu = Gtk::manage(new Gtk::HBox());
    hbu->pack_start(*Gtk::manage(new Gtk::Label(M("TP_BORDER_UNIT") + ": ")),
                    Gtk::PACK_SHRINK);
    unit = Gtk::manage(new MyComboBoxText());
    unit->append(M("TP_BORDER_UNIT_PIXELS"));
    unit->append(M("TP_BORDER_UNIT_PERCENT"));
    unit->set_tooltip_text(M("TP_BORDER_UNIT_TOOLTIP"));
    unit->signal_changed().connect(sigc::mem_fun(*this, &Border::unitChanged));
    hbu->pack_start(*unit);
    hbu->show_all();

    // one set of sliders per unit, so that switching the unit keeps the
    // values of the other one
    size = Gtk::manage(new Adjuster(M("TP_BORDER_SIZE"), 0., 1000., 1., 50.));
    size->set_tooltip_text(M("TP_BORDER_SIZE_TOOLTIP"));
    size->setAdjusterListener(this);
    size->show();

    bottomExtra = Gtk::manage(
        new Adjuster(M("TP_BORDER_BOTTOMEXTRA"), 0., 1000., 1., 0.));
    bottomExtra->set_tooltip_text(M("TP_BORDER_BOTTOMEXTRA_TOOLTIP"));
    bottomExtra->setAdjusterListener(this);
    bottomExtra->show();

    sizePercent = Gtk::manage(
        new Adjuster(M("TP_BORDER_SIZE_PERCENT"), 0., 50., 0.1, 3.));
    sizePercent->set_tooltip_text(M("TP_BORDER_SIZE_PERCENT_TOOLTIP"));
    sizePercent->setAdjusterListener(this);
    sizePercent->show();

    bottomExtraPercent = Gtk::manage(
        new Adjuster(M("TP_BORDER_BOTTOMEXTRA_PERCENT"), 0., 50., 0.1, 0.));
    bottomExtraPercent->set_tooltip_text(
        M("TP_BORDER_BOTTOMEXTRA_PERCENT_TOOLTIP"));
    bottomExtraPercent->setAdjusterListener(this);
    bottomExtraPercent->show();

    pixelBox = Gtk::manage(new Gtk::VBox());
    pixelBox->pack_start(*size);
    pixelBox->pack_start(*bottomExtra);
    pixelBox->set_no_show_all(true);

    percentBox = Gtk::manage(new Gtk::VBox());
    percentBox->pack_start(*sizePercent);
    percentBox->pack_start(*bottomExtraPercent);
    percentBox->set_no_show_all(true);

    Gtk::HBox *hbc = Gtk::manage(new Gtk::HBox());
    hbc->pack_start(*Gtk::manage(new Gtk::Label(M("TP_BORDER_COLOR") + ": ")),
                    Gtk::PACK_SHRINK);
    color = Gtk::manage(new Gtk::ColorButton());
    color->set_use_alpha(false);
    color->set_title(M("TP_BORDER_COLOR"));
    color->signal_color_set().connect(
        sigc::mem_fun(*this, &Border::colorChanged));
    hbc->pack_start(*color);
    hbc->show_all();

    pack_start(*hb);
    pack_start(*hbu);
    pack_start(*pixelBox);
    pack_start(*percentBox);
    pack_start(*hbc);

    unit->set_active(0);
    updateUnitVisibility();
}

void Border::updateUnitVisibility()
{
    const bool percent = unit->get_active_row_number() == 1;
    pixelBox->set_visible(!percent);
    percentBox->set_visible(percent);
}

void Border::read(const ProcParams *pp)
{
    disableListener();

    setEnabled(pp->border.enabled);
    placement->set_active(
        pp->border.placement == BorderParams::Placement::INSIDE ? 1 : 0);
    unit->set_active(pp->border.unit == BorderParams::Unit::PERCENT ? 1 : 0);
    updateUnitVisibility();
    size->setValue(pp->border.width);
    bottomExtra->setValue(pp->border.bottomExtra);
    sizePercent->setValue(pp->border.widthPercent);
    bottomExtraPercent->setValue(pp->border.bottomExtraPercent);
    Gdk::RGBA c;
    c.set_rgba(pp->border.red, pp->border.green, pp->border.blue, 1.0);
    color->set_rgba(c);

    enableListener();
}

void Border::write(ProcParams *pp)
{
    pp->border.enabled = getEnabled();
    pp->border.placement = placement->get_active_row_number() == 1
                               ? BorderParams::Placement::INSIDE
                               : BorderParams::Placement::OUTSIDE;
    pp->border.unit = unit->get_active_row_number() == 1
                          ? BorderParams::Unit::PERCENT
                          : BorderParams::Unit::PIXELS;
    pp->border.width = size->getValue();
    pp->border.bottomExtra = bottomExtra->getValue();
    pp->border.widthPercent = sizePercent->getValue();
    pp->border.bottomExtraPercent = bottomExtraPercent->getValue();
    const Gdk::RGBA c = color->get_rgba();
    pp->border.red = c.get_red();
    pp->border.green = c.get_green();
    pp->border.blue = c.get_blue();
}

void Border::setDefaults(const ProcParams *defParams)
{
    size->setDefault(defParams->border.width);
    bottomExtra->setDefault(defParams->border.bottomExtra);
    sizePercent->setDefault(defParams->border.widthPercent);
    bottomExtraPercent->setDefault(defParams->border.bottomExtraPercent);

    initial_params = defParams->border;
}

void Border::adjusterChanged(Adjuster *a, double newval)
{
    if (listener && getEnabled()) {
        if (a == size || a == sizePercent) {
            listener->panelChanged(EvSize, a->getTextValue());
        } else if (a == bottomExtra || a == bottomExtraPercent) {
            listener->panelChanged(EvBottomExtra, a->getTextValue());
        }
    }
}

void Border::placementChanged()
{
    if (listener && getEnabled()) {
        listener->panelChanged(EvPlacement, placement->get_active_text());
    }
}

void Border::unitChanged()
{
    updateUnitVisibility();
    if (listener && getEnabled()) {
        listener->panelChanged(EvUnit, unit->get_active_text());
    }
}

void Border::colorChanged()
{
    if (listener && getEnabled()) {
        listener->panelChanged(EvColor, color_to_string(color->get_rgba()));
    }
}

void Border::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void Border::toolReset(bool to_initial)
{
    ProcParams pp;
    if (to_initial) {
        pp.border = initial_params;
    }
    pp.border.enabled = getEnabled();
    read(&pp);
}

}} // namespace art::gui
