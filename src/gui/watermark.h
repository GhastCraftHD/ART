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

#include "../engine/watermark.h"
#include "adjuster.h"
#include "edit.h"
#include "guiutils.h"
#include "toolpanel.h"
#include <gtkmm.h>

namespace art { namespace gui {

class Watermark: public ToolParamBlock,
                 public AdjusterListener,
                 public FoldableToolPanel,
                 public EditSubscriber {
public:
    Watermark();
    ~Watermark() override;

    void read(const art::engine::procparams::ProcParams *pp) override;
    void write(art::engine::procparams::ProcParams *pp) override;
    void
    setDefaults(const art::engine::procparams::ProcParams *defParams) override;
    void adjusterChanged(Adjuster *a, double newval) override;
    void adjusterAutoToggled(Adjuster *a, bool newval) override {}
    void enabledChanged() override;
    void toolReset(bool to_initial) override;

    void updateContext(const art::engine::procparams::ProcParams &pp);

    void setEditProvider(EditDataProvider *provider) override;
    CursorShape getCursor(int objectID) override;
    bool mouseOver(int modifierKey) override;
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;
    void switchOffEditMode() override;

private:
    void readParams(const art::engine::procparams::WatermarkParams &wp);
    art::engine::procparams::WatermarkParams getParams();

    void typeChanged();
    void textChanged();
    bool textChangedDelayed();
    void fontChanged();
    void colorChanged();
    void fileChanged();
    void interpolationChanged();
    void unitChanged();
    void anchorChanged();
    void referenceChanged();
    void saveDefaultPressed();
    void loadDefaultPressed();
    void editToggled();

    void updateVisibility();
    void updateFileFilter();
    void showFile();
    void checkSource();

    bool getPlacement(art::engine::WatermarkFrame &frame, double &out_scale,
                      art::engine::WatermarkBox &box);
    void updateGeometry();

    static Glib::ustring defaultsFile();
    static bool loadUserDefaults(art::engine::procparams::WatermarkParams &wp);

    MyComboBoxText *type;

    Gtk::VBox *textBox;
    Gtk::Entry *text;
    Gtk::FontButton *font;
    Gtk::ColorButton *color;

    Gtk::VBox *fileBox;
    MyFileChooserButton *file;
    Glib::RefPtr<Gtk::FileFilter> imageFilter;
    Glib::RefPtr<Gtk::FileFilter> svgFilter;
    Gtk::HBox *fileWarning;
    Gtk::Label *fileWarningLabel;
    Gtk::HBox *interpolationBox;
    MyComboBoxText *interpolation;
    Glib::ustring imageFile;
    Glib::ustring svgFile;

    MyComboBoxText *unit;
    Gtk::VBox *pixelBox;
    Gtk::VBox *percentBox;
    Adjuster *size;
    Adjuster *posX;
    Adjuster *posY;
    Adjuster *sizePercent;
    Adjuster *posXPercent;
    Adjuster *posYPercent;

    MyComboBoxText *anchor;
    Gtk::ToggleButton *edit;
    sigc::connection editConn;
    MyComboBoxText *reference;
    Adjuster *opacity;
    Adjuster *rotation;

    sigc::connection textDelay;

    art::engine::procparams::ProcParams context;

    int lastObject;
    bool dragging;
    double dragStartX;
    double dragStartY;
    int snapCol;
    int snapRow;

    art::engine::ProcEvent EvEnabled;
    art::engine::ProcEvent EvType;
    art::engine::ProcEvent EvText;
    art::engine::ProcEvent EvFont;
    art::engine::ProcEvent EvColor;
    art::engine::ProcEvent EvFile;
    art::engine::ProcEvent EvInterpolation;
    art::engine::ProcEvent EvUnit;
    art::engine::ProcEvent EvSize;
    art::engine::ProcEvent EvPosition;
    art::engine::ProcEvent EvAnchor;
    art::engine::ProcEvent EvReference;
    art::engine::ProcEvent EvOpacity;
    art::engine::ProcEvent EvRotation;
    art::engine::ProcEvent EvDefaults;

    art::engine::procparams::WatermarkParams initial_params;
};

}}
