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

#include "adjuster.h"
#include "toolpanel.h"
#include <gtkmm.h>

namespace art { namespace gui {

class Border: public ToolParamBlock,
              public AdjusterListener,
              public FoldableToolPanel {
public:
    Border();

    void read(const art::engine::procparams::ProcParams *pp) override;
    void write(art::engine::procparams::ProcParams *pp) override;
    void
    setDefaults(const art::engine::procparams::ProcParams *defParams) override;
    void adjusterChanged(Adjuster *a, double newval) override;
    void adjusterAutoToggled(Adjuster *a, bool newval) override {}
    void enabledChanged() override;
    void toolReset(bool to_initial) override;

private:
    void placementChanged();
    void unitChanged();
    void colorChanged();
    void updateUnitVisibility();

    MyComboBoxText *placement;
    MyComboBoxText *unit;
    Gtk::VBox *pixelBox;
    Gtk::VBox *percentBox;
    Adjuster *size;
    Adjuster *bottomExtra;
    Adjuster *sizePercent;
    Adjuster *bottomExtraPercent;
    Gtk::ColorButton *color;

    art::engine::ProcEvent EvEnabled;
    art::engine::ProcEvent EvPlacement;
    art::engine::ProcEvent EvUnit;
    art::engine::ProcEvent EvSize;
    art::engine::ProcEvent EvBottomExtra;
    art::engine::ProcEvent EvColor;

    art::engine::procparams::BorderParams initial_params;
};

}} // namespace art::gui
