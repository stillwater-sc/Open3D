// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2020 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/visualization/visualizer/O3DVisualizer.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "open3d/Open3DConfig.h"
#include "open3d/geometry/Image.h"
#include "open3d/geometry/LineSet.h"
#include "open3d/geometry/PointCloud.h"
#include "open3d/io/ImageIO.h"
#include "open3d/t/geometry/PointCloud.h"
#include "open3d/t/geometry/TriangleMesh.h"
#include "open3d/utility/Console.h"
#include "open3d/utility/FileSystem.h"
#include "open3d/visualization/gui/Application.h"
#include "open3d/visualization/gui/Button.h"
#include "open3d/visualization/gui/Checkbox.h"
#include "open3d/visualization/gui/ColorEdit.h"
#include "open3d/visualization/gui/Combobox.h"
#include "open3d/visualization/gui/Dialog.h"
#include "open3d/visualization/gui/FileDialog.h"
#include "open3d/visualization/gui/Label.h"
#include "open3d/visualization/gui/Layout.h"
#include "open3d/visualization/gui/ListView.h"
#include "open3d/visualization/gui/NumberEdit.h"
#include "open3d/visualization/gui/SceneWidget.h"
#include "open3d/visualization/gui/Slider.h"
#include "open3d/visualization/gui/TabControl.h"
#include "open3d/visualization/gui/Theme.h"
#include "open3d/visualization/gui/TreeView.h"
#include "open3d/visualization/gui/VectorEdit.h"
#include "open3d/visualization/rendering/Open3DScene.h"
#include "open3d/visualization/rendering/Scene.h"
#include "open3d/visualization/visualizer/GuiWidgets.h"
#include "open3d/visualization/visualizer/O3DVisualizerSelections.h"

#define GROUPS_USE_TREE 1

using namespace open3d::visualization::gui;
using namespace open3d::visualization::rendering;

namespace open3d {
namespace visualization {
namespace visualizer {

namespace {
static const std::string kShaderLit = "defaultLit";
static const std::string kShaderUnlit = "defaultUnlit";

static const std::string kDefaultIBL = "default";

enum MenuId {
    MENU_ABOUT = 0,
    MENU_EXPORT_RGB,
    MENU_CLOSE,
    MENU_SETTINGS,
    MENU_ACTIONS_BASE = 1000 /* this should be last */
};

template <typename T>
std::shared_ptr<T> GiveOwnership(T *ptr) {
    return std::shared_ptr<T>(ptr);
}

class AnimationFrameOrder {
public:
    void AddValue(double order) {
        auto it = std::lower_bound(values_.begin(), values_.end(), order);
        if (it == values_.end()) {  // not in list; larger than anything in it
            values_.push_back(order);
        } else if (*it != order) {  // not in list; less than an existing value
            values_.insert(it, order);
        }  // else: already exists in list; do nothing
    }

    void RemoveValue(double order) {
        auto it = std::lower_bound(values_.begin(), values_.end(), order);
        if (it != values_.end()) {
            values_.erase(it);
        }
    }

    size_t GetFrameForValue(double order) {
        auto it = std::lower_bound(values_.begin(), values_.end(), order);
        if (it != values_.end()) {
            return (it - values_.begin());
        } else {
            return -1;
        }
    }

    size_t GetNumberOfFrames() const { return values_.size(); }

private:
    std::vector<double> values_;  // this is ordered in increasing value
};

class ButtonList : public Widget {
public:
    explicit ButtonList(int spacing) : spacing_(spacing) {}

    void SetWidth(int width) { width_ = width; }

    Size CalcPreferredSize(const Theme &theme) const override {
        auto frames = CalcFrames(theme);
        if (!frames.empty()) {
            // Add spacing on the bottom to look like the start of a new row
            return Size(width_,
                        frames.back().GetBottom() - frames[0].y + spacing_);
        } else {
            return Size(width_, 0);
        }
    }

    void Layout(const Theme &theme) override {
        auto frames = CalcFrames(theme);
        auto &children = GetChildren();
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->SetFrame(frames[i]);
        }
    }

    size_t size() const { return GetChildren().size(); }

private:
    int spacing_;
    int width_ = 10000;

    std::vector<Rect> CalcFrames(const Theme &theme) const {
        auto &f = GetFrame();
        std::vector<Rect> frames;
        int x = f.x;
        int y = f.y;
        int lineHeight = 0;
        for (auto child : GetChildren()) {
            auto pref = child->CalcPreferredSize(theme);
            if (x > f.x && x + pref.width > f.x + width_) {
                y = y + lineHeight + spacing_;
                x = f.x;
                lineHeight = 0;
            }
            frames.emplace_back(x, y, pref.width, pref.height);
            x += pref.width + spacing_;
            lineHeight = std::max(lineHeight, pref.height);
        }
        return frames;
    }
};

class EmptyIfHiddenVert : public CollapsableVert {
    using Super = CollapsableVert;

public:
    EmptyIfHiddenVert(const char *text) : CollapsableVert(text) {}
    EmptyIfHiddenVert(const char *text,
                      int spacing,
                      const Margins &margins = Margins())
        : CollapsableVert(text, spacing, margins) {}

    void SetVisible(bool vis) override {
        Super::SetVisible(vis);
        Super::SetIsOpen(vis);
        needsLayout_ = true;
    }

    Size CalcPreferredSize(const Theme &theme) const override {
        if (IsVisible()) {
            return Super::CalcPreferredSize(theme);
        } else {
            return Size(0, 0);
        }
    }

    Widget::DrawResult Draw(const DrawContext &context) override {
        auto result = Super::Draw(context);
        if (needsLayout_) {
            needsLayout_ = false;
            return Widget::DrawResult::RELAYOUT;
        } else {
            return result;
        }
    }

private:
    bool needsLayout_ = false;
};

class DrawObjectTreeCell : public Widget {
    using Super = Widget;

public:
    enum { FLAG_NONE = 0, FLAG_GROUP = (1 << 0), FLAG_ORDER = (1 << 1) };

    DrawObjectTreeCell(const char *name,
                       const char *group,
                       double order,
                       bool is_checked,
                       int flags,
                       std::function<void(bool)> on_toggled) {
        flags_ = flags;

        std::string order_str;
        if (flags & FLAG_ORDER) {
            char buf[32];
            if (order == double(int(order))) {
                snprintf(buf, sizeof(buf), "%d", int(order));
            } else {
                snprintf(buf, sizeof(buf), "%g", order);
            }
            order_str = std::string(buf);
        }

        // We don't want any text in the checkbox, but passing "" seems to make
        // it not toggle, so we need to pass in something. This way it will
        // just be extra spacing.
        checkbox_ = std::make_shared<Checkbox>(" ");
        checkbox_->SetChecked(is_checked);
        checkbox_->SetOnChecked(on_toggled);
        name_ = std::make_shared<Label>(name);
        group_ = std::make_shared<Label>((flags & FLAG_GROUP) ? group : "");
        order_ = std::make_shared<Label>(order_str.c_str());
        AddChild(checkbox_);
        AddChild(name_);
        AddChild(group_);
        AddChild(order_);
    }

    ~DrawObjectTreeCell() {}

    std::shared_ptr<Checkbox> GetCheckbox() { return checkbox_; }
    std::shared_ptr<Label> GetName() { return name_; }

    Size CalcPreferredSize(const Theme &theme) const override {
        auto check_pref = checkbox_->CalcPreferredSize(theme);
        auto name_pref = name_->CalcPreferredSize(theme);
        int w = check_pref.width + name_pref.width + GroupWidth(theme) +
                OrderWidth(theme);
        return Size(w, std::max(check_pref.height, name_pref.height));
    }

    void Layout(const Theme &theme) override {
        auto &frame = GetFrame();
        auto check_width = checkbox_->CalcPreferredSize(theme).width;
        checkbox_->SetFrame(Rect(frame.x, frame.y, check_width, frame.height));
        auto group_width = GroupWidth(theme);
        auto order_width = OrderWidth(theme);
        auto x = checkbox_->GetFrame().GetRight();
        auto name_width = frame.GetRight() - group_width - order_width - x;
        name_->SetFrame(Rect(x, frame.y, name_width, frame.height));
        x += name_width;
        group_->SetFrame(Rect(x, frame.y, group_width, frame.height));
        x += group_width;
        order_->SetFrame(Rect(x, frame.y, order_width, frame.height));
    }

private:
    int flags_;
    std::shared_ptr<Checkbox> checkbox_;
    std::shared_ptr<Label> name_;
    std::shared_ptr<Label> group_;
    std::shared_ptr<Label> order_;

    int GroupWidth(const Theme &theme) const {
        if (flags_ & FLAG_GROUP) {
            return 5 * theme.font_size;
        } else {
            return 0;
        }
    }

    int OrderWidth(const Theme &theme) const {
        if (flags_ & FLAG_ORDER) {
            return 3 * theme.font_size;
        } else {
            return 0;
        }
    }
};

struct LightingProfile {
    std::string name;
    Open3DScene::LightingProfile profile;
};

static const char *kCustomName = "Custom";
static const std::vector<LightingProfile> gLightingProfiles = {
        {"Hard shadows", Open3DScene::LightingProfile::HARD_SHADOWS},
        {"Dark shadows", Open3DScene::LightingProfile::DARK_SHADOWS},
        {"Medium shadows", Open3DScene::LightingProfile::MED_SHADOWS},
        {"Soft shadows", Open3DScene::LightingProfile::SOFT_SHADOWS},
        {"No shadows", Open3DScene::LightingProfile::NO_SHADOWS}};

}  // namespace

struct O3DVisualizer::Impl {
    std::set<std::string> added_names_;
    std::set<std::string> added_groups_;
    std::vector<DrawObject> objects_;
    AnimationFrameOrder frames_;
    std::shared_ptr<O3DVisualizerSelections> selections_;
    bool selections_need_update_ = true;

    UIState ui_state_;
    bool can_auto_show_settings_ = true;
    double next_animation_tick_clock_time_ = 0.0;

    Window *window_ = nullptr;
    SceneWidget *scene_ = nullptr;

    struct {
        // We only keep pointers here because that way we don't have to release
        // all the shared_ptrs at destruction just to ensure that the gui gets
        // destroyed before the Window, because the Window will do that for us.
        Menu *actions_menu;
        std::unordered_map<int, std::function<void(O3DVisualizer &)>>
                menuid2action;

        Vert *panel;
        CollapsableVert *mouse_panel;
        TabControl *mouse_tab;
        Vert *view_panel;
        SceneWidget::Controls view_mouse_mode;
        std::map<SceneWidget::Controls, Button *> mouse_buttons;
        Vert *pick_panel;
        Button *new_selection_set;
        Button *delete_selection_set;
        ListView *selection_sets;

        CollapsableVert *scene_panel;
        Checkbox *show_skybox;
        Checkbox *show_axes;
        ColorEdit *bg_color;
        Slider *point_size;
        Combobox *shader;
        Combobox *lighting;

        CollapsableVert *light_panel;
        Checkbox *use_ibl;
        Checkbox *use_sun;
        Combobox *ibl_names;
        Slider *ibl_intensity;
        Slider *sun_intensity;
        VectorEdit *sun_dir;
        ColorEdit *sun_color;

        CollapsableVert *geometries_panel;
        TreeView *geometries;
#if GROUPS_USE_TREE
        std::map<std::string, TreeView::ItemId> group2itemid;
#endif  // GROUPS_USE_TREE
        std::map<std::string, TreeView::ItemId> object2itemid;

#if !GROUPS_USE_TREE
        EmptyIfHiddenVert *groups_panel;
        TreeView *groups;
#endif  // !GROUPS_USE_TREE

        EmptyIfHiddenVert *anim_panel;
        Slider *anim_slider;
        NumberEdit *anim_edit;
        SmallToggleButton *play;

        EmptyIfHiddenVert *actions_panel;
        ButtonList *actions;
    } settings;

    void Construct(O3DVisualizer *w) {
        if (window_) {
            return;
        }

        window_ = w;
        scene_ = new SceneWidget();
        selections_ = std::make_shared<O3DVisualizerSelections>(*scene_);
        scene_->SetScene(std::make_shared<Open3DScene>(w->GetRenderer()));
        scene_->EnableSceneCaching(true);  // smoother UI with large geometry
        scene_->SetOnPointsPicked(
                [this](const std::vector<size_t> &indices, int keymods) {
                    if (keymods & int(KeyModifier::SHIFT)) {
                        selections_->UnselectIndices(indices);
                    } else {
                        selections_->SelectIndices(indices);
                    }
                });
        w->AddChild(GiveOwnership(scene_));

        auto o3dscene = scene_->GetScene();
        o3dscene->SetBackgroundColor(ui_state_.bg_color);

        MakeSettingsUI();
        SetMouseMode(SceneWidget::Controls::ROTATE_CAMERA);
        SetLightingProfile(gLightingProfiles[2]);  // med shadows
        SetPointSize(ui_state_.point_size);  // sync selections_' point size
    }

    void MakeSettingsUI() {
        auto em = window_->GetTheme().font_size;
        auto half_em = int(std::round(0.5f * float(em)));
        auto v_spacing = int(std::round(0.25 * float(em)));

        settings.panel = new Vert(half_em);
        window_->AddChild(GiveOwnership(settings.panel));

        Margins margins(em, 0, half_em, 0);
        Margins tabbed_margins(0, half_em, 0, 0);

        settings.mouse_panel =
                new CollapsableVert("Mouse Controls", v_spacing, margins);
        settings.panel->AddChild(GiveOwnership(settings.mouse_panel));

        settings.mouse_tab = new TabControl();
        settings.mouse_panel->AddChild(GiveOwnership(settings.mouse_tab));

        settings.view_panel = new Vert(v_spacing, tabbed_margins);
        settings.pick_panel = new Vert(v_spacing, tabbed_margins);
        settings.mouse_tab->AddTab("Scene", GiveOwnership(settings.view_panel));
        settings.mouse_tab->AddTab("Selection",
                                   GiveOwnership(settings.pick_panel));
        settings.mouse_tab->SetOnSelectedTabChanged([this](int tab_idx) {
            if (tab_idx == 0) {
                SetMouseMode(settings.view_mouse_mode);
            } else {
                SetPicking();
            }
        });

        // Mouse countrols
        auto MakeMouseButton = [this](const char *name,
                                      SceneWidget::Controls type) {
            auto button = new SmallToggleButton(name);
            button->SetOnClicked([this, type]() { this->SetMouseMode(type); });
            this->settings.mouse_buttons[type] = button;
            return button;
        };
        auto *h = new Horiz(v_spacing);
        h->AddStretch();
        h->AddChild(GiveOwnership(MakeMouseButton(
                "Arcball", SceneWidget::Controls::ROTATE_CAMERA)));
        h->AddChild(GiveOwnership(
                MakeMouseButton("Fly", SceneWidget::Controls::FLY)));
        h->AddChild(GiveOwnership(
                MakeMouseButton("Model", SceneWidget::Controls::ROTATE_MODEL)));
        h->AddStretch();
        settings.view_panel->AddChild(GiveOwnership(h));

        h = new Horiz(v_spacing);
        h->AddStretch();
        h->AddChild(GiveOwnership(MakeMouseButton(
                "Sun Direction", SceneWidget::Controls::ROTATE_SUN)));
        h->AddChild(GiveOwnership(MakeMouseButton(
                "Environment", SceneWidget::Controls::ROTATE_IBL)));
        h->AddStretch();
        settings.view_panel->AddChild(GiveOwnership(h));
        settings.view_panel->AddFixed(half_em);

        auto *reset = new SmallButton("Reset Camera");
        reset->SetOnClicked([this]() { this->ResetCameraToDefault(); });

        h = new Horiz(v_spacing);
        h->AddStretch();
        h->AddChild(GiveOwnership(reset));
        h->AddStretch();
        settings.view_panel->AddChild(GiveOwnership(h));

        // Selection sets controls
        settings.new_selection_set = new SmallButton(" + ");
        settings.new_selection_set->SetOnClicked(
                [this]() { NewSelectionSet(); });
        settings.delete_selection_set = new SmallButton(" - ");
        settings.delete_selection_set->SetOnClicked([this]() {
            int idx = settings.selection_sets->GetSelectedIndex();
            RemoveSelectionSet(idx);
        });
        settings.selection_sets = new ListView();
        settings.selection_sets->SetOnValueChanged([this](const char *, bool) {
            SelectSelectionSet(settings.selection_sets->GetSelectedIndex());
        });

#if __APPLE__
        const char *selection_help = "Cmd-click to select a point";
#else
        const char *selection_help = "Ctrl-click to select a point";
#endif  // __APPLE__
        h = new Horiz();
        h->AddStretch();
        h->AddChild(std::make_shared<Label>(selection_help));
        h->AddStretch();
        settings.pick_panel->AddChild(GiveOwnership(h));
        h = new Horiz(v_spacing);
        h->AddChild(std::make_shared<Label>("Selection Sets"));
        h->AddStretch();
        h->AddChild(GiveOwnership(settings.new_selection_set));
        h->AddChild(GiveOwnership(settings.delete_selection_set));
        settings.pick_panel->AddChild(GiveOwnership(h));
        settings.pick_panel->AddChild(GiveOwnership(settings.selection_sets));

        // Scene controls
        settings.scene_panel = new CollapsableVert("Scene", v_spacing, margins);
        settings.panel->AddChild(GiveOwnership(settings.scene_panel));

        settings.show_skybox = new Checkbox("Show Skybox");
        settings.show_skybox->SetOnChecked(
                [this](bool is_checked) { this->ShowSkybox(is_checked); });

        settings.show_axes = new Checkbox("Show Axis");
        settings.show_axes->SetOnChecked(
                [this](bool is_checked) { this->ShowAxes(is_checked); });

        h = new Horiz(v_spacing);
        h->AddChild(GiveOwnership(settings.show_axes));
        h->AddFixed(em);
        h->AddChild(GiveOwnership(settings.show_skybox));
        settings.scene_panel->AddChild(GiveOwnership(h));

        settings.bg_color = new ColorEdit();
        settings.bg_color->SetValue(ui_state_.bg_color.x(),
                                    ui_state_.bg_color.y(),
                                    ui_state_.bg_color.z());
        settings.bg_color->SetOnValueChanged([this](const Color &c) {
            this->SetBackgroundColor(
                    {c.GetRed(), c.GetGreen(), c.GetBlue(), 1.0f});
        });

        settings.point_size = new Slider(Slider::INT);
        settings.point_size->SetLimits(1, 10);
        settings.point_size->SetValue(ui_state_.point_size);
        settings.point_size->SetOnValueChanged([this](const double newValue) {
            this->SetPointSize(int(newValue));
        });

        settings.shader = new Combobox();
        settings.shader->AddItem("Standard");
        settings.shader->AddItem("Normal Map");
        settings.shader->AddItem("Depth");
        settings.shader->SetOnValueChanged([this](const char *item, int idx) {
            if (idx == 1) {
                this->SetShader(O3DVisualizer::Shader::NORMALS);
            } else if (idx == 2) {
                this->SetShader(O3DVisualizer::Shader::DEPTH);
            } else {
                this->SetShader(O3DVisualizer::Shader::STANDARD);
            }
        });

        settings.lighting = new Combobox();
        for (auto &profile : gLightingProfiles) {
            settings.lighting->AddItem(profile.name.c_str());
        }
        settings.lighting->AddItem(kCustomName);
        settings.lighting->SetOnValueChanged([this](const char *, int index) {
            if (index < int(gLightingProfiles.size())) {
                this->SetLightingProfile(gLightingProfiles[index]);
            }
        });

        auto *grid = new VGrid(2, v_spacing);
        settings.scene_panel->AddChild(GiveOwnership(grid));

        grid->AddChild(std::make_shared<Label>("BG Color"));
        grid->AddChild(GiveOwnership(settings.bg_color));
        grid->AddChild(std::make_shared<Label>("PointSize"));
        grid->AddChild(GiveOwnership(settings.point_size));
        grid->AddChild(std::make_shared<Label>("Shader"));
        grid->AddChild(GiveOwnership(settings.shader));
        grid->AddChild(std::make_shared<Label>("Lighting"));
        grid->AddChild(GiveOwnership(settings.lighting));

        // Light list
        settings.light_panel = new CollapsableVert("Lighting", 0, margins);
        settings.light_panel->SetIsOpen(false);
        settings.panel->AddChild(GiveOwnership(settings.light_panel));

        h = new Horiz(v_spacing);
        settings.use_ibl = new Checkbox("HDR map");
        settings.use_ibl->SetChecked(ui_state_.use_ibl);
        settings.use_ibl->SetOnChecked([this](bool checked) {
            this->ui_state_.use_ibl = checked;
            this->SetUIState(ui_state_);
            this->settings.lighting->SetSelectedValue(kCustomName);
        });

        settings.use_sun = new Checkbox("Sun");
        settings.use_sun->SetChecked(settings.use_sun);
        settings.use_sun->SetOnChecked([this](bool checked) {
            this->ui_state_.use_sun = checked;
            this->SetUIState(ui_state_);
            this->settings.lighting->SetSelectedValue(kCustomName);
        });

        h->AddChild(GiveOwnership(settings.use_ibl));
        h->AddFixed(int(std::round(
                1.4 * em)));  // align with Show Skybox checkbox above
        h->AddChild(GiveOwnership(settings.use_sun));

        settings.light_panel->AddChild(
                std::make_shared<Label>("Light sources"));
        settings.light_panel->AddChild(GiveOwnership(h));
        settings.light_panel->AddFixed(half_em);

        grid = new VGrid(2, v_spacing);

        settings.ibl_names = new Combobox();
        for (auto &name : GetListOfIBLs()) {
            settings.ibl_names->AddItem(name.c_str());
        }
        settings.ibl_names->SetSelectedValue(kDefaultIBL.c_str());
        settings.ibl_names->SetOnValueChanged([this](const char *val, int idx) {
            std::string resource_path =
                    Application::GetInstance().GetResourcePath();
            this->SetIBL(resource_path + std::string("/") + std::string(val));
            this->settings.lighting->SetSelectedValue(kCustomName);
        });
        grid->AddChild(std::make_shared<Label>("HDR map"));
        grid->AddChild(GiveOwnership(settings.ibl_names));

        settings.ibl_intensity = new Slider(Slider::INT);
        settings.ibl_intensity->SetLimits(0.0, 150000.0);
        settings.ibl_intensity->SetValue(ui_state_.ibl_intensity);
        settings.ibl_intensity->SetOnValueChanged([this](double new_value) {
            this->ui_state_.ibl_intensity = int(new_value);
            this->SetUIState(ui_state_);
            this->settings.lighting->SetSelectedValue(kCustomName);
        });
        grid->AddChild(std::make_shared<Label>("Intensity"));
        grid->AddChild(GiveOwnership(settings.ibl_intensity));

        settings.light_panel->AddChild(std::make_shared<Label>("Environment"));
        settings.light_panel->AddChild(GiveOwnership(grid));
        settings.light_panel->AddFixed(half_em);

        grid = new VGrid(2, v_spacing);

        settings.sun_intensity = new Slider(Slider::INT);
        settings.sun_intensity->SetLimits(0.0, 150000.0);
        settings.sun_intensity->SetValue(ui_state_.ibl_intensity);
        settings.sun_intensity->SetOnValueChanged([this](double new_value) {
            this->ui_state_.sun_intensity = int(new_value);
            this->SetUIState(ui_state_);
            this->settings.lighting->SetSelectedValue(kCustomName);
        });
        grid->AddChild(std::make_shared<Label>("Intensity"));
        grid->AddChild(GiveOwnership(settings.sun_intensity));

        settings.sun_dir = new VectorEdit();
        settings.sun_dir->SetValue(ui_state_.sun_dir);
        settings.sun_dir->SetOnValueChanged([this](const Eigen::Vector3f &dir) {
            this->ui_state_.sun_dir = dir;
            this->SetUIState(ui_state_);
            this->settings.lighting->SetSelectedValue(kCustomName);
        });
        scene_->SetOnSunDirectionChanged(
                [this](const Eigen::Vector3f &new_dir) {
                    this->ui_state_.sun_dir = new_dir;
                    this->settings.sun_dir->SetValue(new_dir);
                    // Don't need to call SetUIState(), the SceneWidget already
                    // modified the scene.
                    this->settings.lighting->SetSelectedValue(kCustomName);
                });
        grid->AddChild(std::make_shared<Label>("Direction"));
        grid->AddChild(GiveOwnership(settings.sun_dir));

        settings.sun_color = new ColorEdit();
        settings.sun_color->SetValue(ui_state_.sun_color);
        settings.sun_color->SetOnValueChanged([this](const Color &new_color) {
            this->ui_state_.sun_color = {new_color.GetRed(),
                                         new_color.GetGreen(),
                                         new_color.GetBlue()};
            this->SetUIState(ui_state_);
            this->settings.lighting->SetSelectedValue(kCustomName);
        });
        grid->AddChild(std::make_shared<Label>("Color"));
        grid->AddChild(GiveOwnership(settings.sun_color));

        settings.light_panel->AddChild(
                std::make_shared<Label>("Sun (Directional light)"));
        settings.light_panel->AddChild(GiveOwnership(grid));

        // Geometry list
        settings.geometries_panel =
                new CollapsableVert("Geometries", v_spacing, margins);
        settings.panel->AddChild(GiveOwnership(settings.geometries_panel));

        settings.geometries = new TreeView();
        settings.geometries_panel->AddChild(GiveOwnership(settings.geometries));

#if !GROUPS_USE_TREE
        // Groups
        settings.groups_panel =
                new EmptyIfHiddenVert("Groups", v_spacing, margins);
        settings.panel->AddChild(GiveOwnership(settings.groups_panel));

        settings.groups = new TreeView();
        settings.groups_panel->AddChild(GiveOwnership(settings.groups));

        settings.groups_panel->SetVisible(false);
#endif  // !GROUPS_USE_TREE

        // Time controls
        settings.anim_panel =
                new EmptyIfHiddenVert("Animation", v_spacing, margins);
        settings.panel->AddChild(GiveOwnership(settings.anim_panel));

        settings.anim_slider = new Slider(Slider::INT);
        settings.anim_slider->SetOnValueChanged([this](double new_value) {
            this->ui_state_.current_frame = int(new_value);
            this->UpdateFrameUI();
            this->SetCurrentFrame(int(new_value));
        });

        settings.anim_edit = new NumberEdit(NumberEdit::INT);
        settings.anim_edit->SetOnValueChanged([this](double new_value) {
            this->ui_state_.current_frame = int(new_value);
            this->UpdateFrameUI();
            this->SetCurrentFrame(new_value);
        });

        settings.play = new SmallToggleButton("Play");
        settings.play->SetOnClicked(
                [this]() { this->SetAnimating(settings.play->GetIsOn()); });

        h = new Horiz(v_spacing);
        h->AddChild(GiveOwnership(settings.anim_slider));
        h->AddChild(GiveOwnership(settings.anim_edit));
        h->AddChild(GiveOwnership(settings.play));
        settings.anim_panel->AddChild(GiveOwnership(h));

        settings.anim_panel->SetVisible(false);  // hide until we add a
                                                 // geometry with time

        // Custom actions
        settings.actions_panel =
                new EmptyIfHiddenVert("Custom Actions", v_spacing, margins);
        settings.panel->AddChild(GiveOwnership(settings.actions_panel));
        settings.actions_panel->SetVisible(false);

        settings.actions = new ButtonList(v_spacing);
        settings.actions_panel->AddChild(GiveOwnership(settings.actions));
    }

    void AddGeometry(const std::string &name,
                     std::shared_ptr<geometry::Geometry3D> geom,
                     std::shared_ptr<t::geometry::Geometry> tgeom,
                     rendering::Material *material,
                     const std::string &group,
                     double order,
                     bool is_visible) {
        std::string group_name = group;
        if (group_name == "") {
            group_name = "default";
        }
        bool is_default_color;
        Material mat;
        if (material) {
            mat = *material;
            is_default_color = false;
        } else {
            bool has_colors = false;
            bool has_normals = false;

            auto cloud = std::dynamic_pointer_cast<geometry::PointCloud>(geom);
            auto lines = std::dynamic_pointer_cast<geometry::LineSet>(geom);
            auto obb = std::dynamic_pointer_cast<geometry::OrientedBoundingBox>(
                    geom);
            auto aabb =
                    std::dynamic_pointer_cast<geometry::AxisAlignedBoundingBox>(
                            geom);
            auto mesh = std::dynamic_pointer_cast<geometry::MeshBase>(geom);

            auto t_cloud =
                    std::dynamic_pointer_cast<t::geometry::PointCloud>(tgeom);
            auto t_mesh =
                    std::dynamic_pointer_cast<t::geometry::TriangleMesh>(tgeom);

            if (cloud) {
                has_colors = !cloud->colors_.empty();
                has_normals = !cloud->normals_.empty();
            } else if (t_cloud) {
                has_colors = t_cloud->HasPointColors();
                has_normals = t_cloud->HasPointNormals();
            } else if (lines) {
                has_colors = !lines->colors_.empty();
            } else if (obb) {
                has_colors = (obb->color_ != Eigen::Vector3d{0.0, 0.0, 0.0});
            } else if (aabb) {
                has_colors = (aabb->color_ != Eigen::Vector3d{0.0, 0.0, 0.0});
            } else if (mesh) {
                has_normals = !mesh->vertex_normals_.empty();
                has_colors = true;  // always want base_color as white
            } else if (t_mesh) {
                has_normals = !t_mesh->HasVertexNormals();
                has_colors = true;  // always want base_color as white
            }

            mat.base_color = CalcDefaultUnlitColor();
            mat.shader = kShaderUnlit;
            is_default_color = true;
            if (has_colors) {
                mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
                is_default_color = false;
            }
            if (has_normals) {
                mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
                mat.shader = kShaderLit;
                is_default_color = false;
            }
            mat.point_size = ui_state_.point_size * window_->GetScaling();
        }

        // We assume that the caller isn't setting a group or order (and in any
        // case we don't know beforehand what they will do). So if they do,
        // we need to update the geometry tree accordingly. This needs to happen
        // before we add the object to the list, otherwise when we regenerate
        // the object will already be added in the list and then get added again
        // below.
        AddGroup(group_name);  // regenerates if necessary

        auto orig_n_frames = frames_.GetNumberOfFrames();
        frames_.AddValue(order);
        bool update_for_order = (orig_n_frames < frames_.GetNumberOfFrames());
        if (update_for_order) {
            settings.anim_slider->SetLimits(0, frames_.GetNumberOfFrames() - 1);
            settings.anim_edit->SetLimits(0, frames_.GetNumberOfFrames() - 1);
            settings.anim_panel->SetVisible(true);
            UpdateObjectTree();
        }

        // Auto-open the settings panel if we set anything fancy that would
        // imply using the UI.
        if (can_auto_show_settings_ &&
            (added_groups_.size() == 2 || update_for_order)) {
            ShowSettings(true);
        }

        objects_.push_back({name, geom, tgeom, mat, group_name, order,
                            is_visible, is_default_color});
        AddObjectToTree(objects_.back());

        auto scene = scene_->GetScene();
        scene->AddGeometry(name, geom, mat);
        UpdateGeometryVisibility(objects_.back());

        scene_->ForceRedraw();
    }

    void RemoveGeometry(const std::string &name) {
        std::string group;
        double order = -1e30;
        for (size_t i = 0; i < objects_.size(); ++i) {
            if (objects_[i].name == name) {
                group = objects_[i].group;
                order = objects_[i].order;
                objects_.erase(objects_.begin() + i);
                settings.object2itemid.erase(objects_[i].name);
                break;
            }
        }

        // Need to check group membership in case this was the last item in its
        // group. Also need to find the number of items with this frame order.
        std::set<std::string> groups;
        int n_others_with_this_order = 0;
        for (auto &o : objects_) {
            groups.insert(o.group);
            if (o.order == order) {
                n_others_with_this_order += 1;
            }
        }

        if (n_others_with_this_order == 0) {
            frames_.RemoveValue(order);
        }
        if (frames_.GetNumberOfFrames() <= 1) {
            SetAnimating(false);
        }
        settings.anim_slider->SetLimits(0, frames_.GetNumberOfFrames() - 1);
        settings.anim_edit->SetLimits(0, frames_.GetNumberOfFrames() - 1);
        SetCurrentFrame(ui_state_.current_frame);  // makes current frame valid

        added_groups_ = groups;
        std::set<std::string> enabled;
        for (auto &g : ui_state_.enabled_groups) {  // remove deleted groups
            if (groups.find(g) != groups.end()) {
                enabled.insert(g);
            }
        }
        ui_state_.enabled_groups = enabled;
        UpdateObjectTree();
        scene_->GetScene()->RemoveGeometry(name);
        scene_->ForceRedraw();
    }

    void ShowGeometry(const std::string &name, bool show) {
        for (auto &o : objects_) {
            if (o.name == name) {
                if (show != o.is_visible) {
                    o.is_visible = show;

                    auto id = settings.object2itemid[o.name];
                    auto cell = settings.geometries->GetItem(id);
                    auto obj_cell =
                            std::dynamic_pointer_cast<DrawObjectTreeCell>(cell);
                    if (obj_cell) {
                        obj_cell->GetCheckbox()->SetChecked(show);
                    }

                    UpdateGeometryVisibility(o);  // calls ForceRedraw()
                    window_->PostRedraw();

                    if (selections_->IsActive()) {
                        UpdateSelectablePoints();
                    } else {
                        selections_need_update_ = true;
                    }
                }
                break;
            }
        }
    }

    O3DVisualizer::DrawObject GetGeometry(const std::string &name) const {
        for (auto &o : objects_) {
            if (o.name == name) {
                return o;
            }
        }
        return DrawObject();
    }

    void SetupCamera(float fov,
                     const Eigen::Vector3f &center,
                     const Eigen::Vector3f &eye,
                     const Eigen::Vector3f &up) {
        auto scene = scene_->GetScene();
        scene_->SetupCamera(fov, scene->GetBoundingBox(), {0.0f, 0.0f, 0.0f});
        scene->GetCamera()->LookAt(center, eye, up);
        scene_->ForceRedraw();
    }

    void ResetCameraToDefault() {
        auto scene = scene_->GetScene();
        scene_->SetupCamera(60.0f, scene->GetBoundingBox(), {0.0f, 0.0f, 0.0f});
        scene_->ForceRedraw();
    }

    void SetBackgroundColor(const Eigen::Vector4f &bg_color) {
        auto old_default_color = CalcDefaultUnlitColor();
        ui_state_.bg_color = bg_color;
        auto scene = scene_->GetScene();
        scene->SetBackgroundColor(ui_state_.bg_color);

        auto new_default_color = CalcDefaultUnlitColor();
        if (new_default_color != old_default_color) {
            for (auto &o : objects_) {
                if (o.is_color_default) {
                    o.material.base_color = new_default_color;
                    scene->GetScene()->OverrideMaterial(o.name, o.material);
                }
            }
        }

        scene_->ForceRedraw();
    }

    void ShowSettings(bool show) {
        can_auto_show_settings_ = false;
        ui_state_.show_settings = show;
        settings.panel->SetVisible(show);
    }

    void ShowSkybox(bool show) {
        ui_state_.show_skybox = show;
        settings.show_skybox->SetChecked(show);  // in case called manually
        scene_->GetScene()->ShowSkybox(show);
        scene_->ForceRedraw();
    }

    void ShowAxes(bool show) {
        ui_state_.show_axes = show;
        settings.show_axes->SetChecked(show);  // in case called manually
        scene_->GetScene()->ShowAxes(show);
        scene_->ForceRedraw();
    }

    void SetPointSize(int px) {
        ui_state_.point_size = px;
        settings.point_size->SetValue(double(px));

        px = int(std::round(px * window_->GetScaling()));
        for (auto &o : objects_) {
            o.material.point_size = float(px);
            scene_->GetScene()->GetScene()->OverrideMaterial(o.name,
                                                             o.material);
        }
        selections_->SetPointSize(px);

        scene_->SetPickablePointSize(px);
        scene_->ForceRedraw();
    }

    void SetShader(O3DVisualizer::Shader shader) {
        const char *shader_name = nullptr;
        switch (shader) {
            case Shader::STANDARD:
                break;
            case Shader::NORMALS:
                shader_name = "normals";
                break;
            case Shader::DEPTH:
                shader_name = "depth";
                break;
        }
        auto scene = scene_->GetScene();
        if (shader_name) {
            Material mat;
            mat.shader = shader_name;
            scene->UpdateMaterial(mat);
        } else {
            for (auto &o : objects_) {
                scene->GetScene()->OverrideMaterial(o.name, o.material);
            }
        }
        scene_->ForceRedraw();
    }

    void SetIBL(std::string path) {
        if (path == "") {
            path = std::string(Application::GetInstance().GetResourcePath()) +
                   std::string("/") + std::string(kDefaultIBL);
        }
        if (utility::filesystem::FileExists(path + "_ibl.ktx")) {
            scene_->GetScene()->GetScene()->SetIndirectLight(path);
            scene_->ForceRedraw();
            ui_state_.ibl_path = path;
        } else if (utility::filesystem::FileExists(path)) {
            if (path.find("_ibl.ktx") == path.size() - 8) {
                ui_state_.ibl_path = path.substr(0, path.size() - 8);
                scene_->GetScene()->GetScene()->SetIndirectLight(
                        ui_state_.ibl_path);
                scene_->ForceRedraw();
            } else {
                utility::LogWarning(
                        "Could not load IBL path. Filename must be of the form "
                        "'name_ibl.ktx' and be paired with 'name_skybox.ktx'");
            }
        }
    }

    void SetLightingProfile(const LightingProfile &profile) {
        Eigen::Vector3f sun_dir = {0.577f, -0.577f, -0.577f};
        auto scene = scene_->GetScene();
        scene->SetLighting(profile.profile, sun_dir);
        ui_state_.use_ibl =
                (profile.profile != Open3DScene::LightingProfile::HARD_SHADOWS);
        ui_state_.use_sun =
                (profile.profile != Open3DScene::LightingProfile::NO_SHADOWS);
        ui_state_.ibl_intensity =
                int(scene->GetScene()->GetIndirectLightIntensity());
        ui_state_.sun_intensity =
                int(scene->GetScene()->GetDirectionalLightIntensity());
        ui_state_.sun_dir = sun_dir;
        ui_state_.sun_color = {1.0f, 1.0f, 1.0f};
        SetUIState(ui_state_);
        // SetUIState will set the combobox to "Custom", so undo that.
        this->settings.lighting->SetSelectedValue(profile.name.c_str());
    }

    void SetMouseMode(SceneWidget::Controls mode) {
        if (selections_->IsActive()) {
            selections_->MakeInactive();
        }

        scene_->SetViewControls(mode);
        settings.view_mouse_mode = mode;
        for (const auto &t_b : settings.mouse_buttons) {
            t_b.second->SetOn(false);
        }
        settings.mouse_buttons[mode]->SetOn(true);
    }

    void SetPicking() {
        if (selections_->GetNumberOfSets() == 0) {
            NewSelectionSet();
        }
        if (selections_need_update_) {
            UpdateSelectablePoints();
        }
        selections_->MakeActive();
    }

    std::vector<O3DVisualizerSelections::SelectionSet> GetSelectionSets()
            const {
        return selections_->GetSets();
    }

    void SetCurrentFrame(size_t f) {
        ui_state_.current_frame = f;
        if (ui_state_.current_frame >= frames_.GetNumberOfFrames()) {
            ui_state_.current_frame = 0;
        }
        for (auto &o : objects_) {
            UpdateGeometryVisibility(o);
        }
        settings.anim_slider->SetValue(ui_state_.current_frame);
        settings.anim_edit->SetValue(ui_state_.current_frame);
    }

    void SetAnimating(bool is_animating) {
        if (is_animating == ui_state_.is_animating) {
            return;
        }

        ui_state_.is_animating = is_animating;
        if (is_animating) {
            ui_state_.current_frame = frames_.GetNumberOfFrames();
            window_->SetOnTickEvent(
                    [this]() -> bool { return this->OnAnimationTick(); });
        } else {
            window_->SetOnTickEvent(nullptr);
            SetCurrentFrame(0);
        }
        settings.anim_slider->SetEnabled(!is_animating);
        settings.anim_edit->SetEnabled(!is_animating);
    }

    void SetUIState(const UIState &new_state) {
        int point_size_changed = (new_state.point_size != ui_state_.point_size);
        bool ibl_path_changed = (new_state.ibl_path != ui_state_.ibl_path);
        auto old_enabled_groups = ui_state_.enabled_groups;
        bool old_is_animating = ui_state_.is_animating;
        bool new_is_animating = new_state.is_animating;
        bool is_new_lighting =
                (ibl_path_changed || new_state.use_ibl != ui_state_.use_ibl ||
                 new_state.use_sun != ui_state_.use_sun ||
                 new_state.ibl_intensity != ui_state_.ibl_intensity ||
                 new_state.sun_intensity != ui_state_.sun_intensity ||
                 new_state.sun_dir != ui_state_.sun_dir ||
                 new_state.sun_color != ui_state_.sun_color);

        if (&new_state != &ui_state_) {
            ui_state_ = new_state;
        }

        if (ibl_path_changed) {
            SetIBL(ui_state_.ibl_path);
        }

        settings.panel->SetVisible(ui_state_.show_settings);
        SetShader(ui_state_.scene_shader);
        SetBackgroundColor(ui_state_.bg_color);
        ShowSkybox(ui_state_.show_skybox);
        ShowAxes(ui_state_.show_axes);

        if (point_size_changed) {
            SetPointSize(ui_state_.point_size);
        }

        settings.use_ibl->SetChecked(ui_state_.use_ibl);
        settings.use_sun->SetChecked(ui_state_.use_sun);
        settings.ibl_intensity->SetValue(ui_state_.ibl_intensity);
        settings.sun_intensity->SetValue(ui_state_.sun_intensity);
        settings.sun_dir->SetValue(ui_state_.sun_dir);
        settings.sun_color->SetValue(ui_state_.sun_color);
        // Re-assign intensity in case it was out of range.
        ui_state_.ibl_intensity = settings.ibl_intensity->GetIntValue();
        ui_state_.sun_intensity = settings.sun_intensity->GetIntValue();

        if (is_new_lighting) {
            settings.lighting->SetSelectedValue(kCustomName);
        }

        auto *raw_scene = scene_->GetScene()->GetScene();
        raw_scene->EnableIndirectLight(ui_state_.use_ibl);
        raw_scene->SetIndirectLightIntensity(float(ui_state_.ibl_intensity));
        raw_scene->EnableDirectionalLight(ui_state_.use_sun);
        raw_scene->SetDirectionalLight(ui_state_.sun_dir, ui_state_.sun_color,
                                       float(ui_state_.sun_intensity));

        if (old_enabled_groups != ui_state_.enabled_groups) {
            for (auto &group : added_groups_) {
                bool enabled = (ui_state_.enabled_groups.find(group) !=
                                ui_state_.enabled_groups.end());
                EnableGroup(group, enabled);
            }
        }

        if (old_is_animating != new_is_animating) {
            ui_state_.is_animating = old_is_animating;
            SetAnimating(new_is_animating);
        }

        scene_->ForceRedraw();
    }

    void AddGroup(const std::string &group) {
#if GROUPS_USE_TREE
        if (added_groups_.find(group) == added_groups_.end()) {
            added_groups_.insert(group);
            ui_state_.enabled_groups.insert(group);
        }
        if (added_groups_.size() == 2) {
            UpdateObjectTree();
        }
#else
        if (added_groups_.find(group) == added_groups_.end()) {
            added_groups_.insert(group);
            ui_state_.enabled_groups.insert(group);

            auto cell = std::make_shared<CheckableTextTreeCell>(
                    group.c_str(), true, [this, group](bool is_on) {
                        this->EnableGroup(group, is_on);
                    });
            auto root = settings.groups->GetRootItem();
            settings.groups->AddItem(root, cell);
        }
        if (added_groups_.size() >= 2) {
            settings.groups_panel->SetVisible(true);
        }
#endif  // GROUPS_USE_TREE
    }

    void EnableGroup(const std::string &group, bool enable) {
#if GROUPS_USE_TREE
        auto group_it = settings.group2itemid.find(group);
        if (group_it != settings.group2itemid.end()) {
            auto cell = settings.geometries->GetItem(group_it->second);
            auto group_cell =
                    std::dynamic_pointer_cast<CheckableTextTreeCell>(cell);
            if (group_cell) {
                group_cell->GetCheckbox()->SetChecked(enable);
            }
        }
#endif  // GROUPS_USE_TREE
        if (enable) {
            ui_state_.enabled_groups.insert(group);
        } else {
            ui_state_.enabled_groups.erase(group);
        }
        for (auto &o : objects_) {
            UpdateGeometryVisibility(o);
        }
    }

    void AddObjectToTree(const DrawObject &o) {
        TreeView::ItemId parent = settings.geometries->GetRootItem();
#if GROUPS_USE_TREE
        if (added_groups_.size() >= 2) {
            auto it = settings.group2itemid.find(o.group);
            if (it != settings.group2itemid.end()) {
                parent = it->second;
            } else {
                auto cell = std::make_shared<CheckableTextTreeCell>(
                        o.group.c_str(), true,
                        [this, group = o.group](bool is_on) {
                            this->EnableGroup(group, is_on);
                        });
                parent = settings.geometries->AddItem(parent, cell);
                settings.group2itemid[o.group] = parent;
            }
        }
#endif  // GROUPS_USE_TREE

        int flag = DrawObjectTreeCell::FLAG_NONE;
#if !GROUPS_USE_TREE
        flag |= (added_groups_.size() >= 2 ? DrawObjectTreeCell::FLAG_GROUP
                                           : 0);
#endif  // !GROUPS_USE_TREE
        flag |= ((frames_.GetNumberOfFrames() > 1)
                         ? DrawObjectTreeCell::FLAG_ORDER
                         : 0);
        auto cell = std::make_shared<DrawObjectTreeCell>(
                o.name.c_str(), o.group.c_str(), o.order, o.is_visible, flag,
                [this, name = o.name](bool is_on) {
                    ShowGeometry(name, is_on);
                });
        auto id = settings.geometries->AddItem(parent, cell);
        settings.object2itemid[o.name] = id;
    }

    void UpdateObjectTree() {
#if GROUPS_USE_TREE
        settings.group2itemid.clear();
#endif  // GROUPS_USE_TREE
        settings.object2itemid.clear();
        settings.geometries->Clear();

        for (auto &o : objects_) {
            AddObjectToTree(o);
        }
    }

    void UpdateFrameUI() {
        settings.anim_slider->SetValue(ui_state_.current_frame);
        settings.anim_edit->SetValue(ui_state_.current_frame);
    }

    void UpdateGeometryVisibility(const DrawObject &o) {
        scene_->GetScene()->ShowGeometry(o.name, IsGeometryVisible(o));
        scene_->ForceRedraw();
    }

    bool IsGeometryVisible(const DrawObject &o) {
        bool is_current =
                (frames_.GetFrameForValue(o.order) == ui_state_.current_frame);
        bool is_group_enabled = (ui_state_.enabled_groups.find(o.group) !=
                                 ui_state_.enabled_groups.end());
        bool is_visible = o.is_visible;
        return (is_visible & is_current & is_group_enabled);
    }

    void NewSelectionSet() {
        selections_->NewSet();
        UpdateSelectionSetList();
        SelectSelectionSet(int(selections_->GetNumberOfSets()) - 1);
    }

    void RemoveSelectionSet(int index) {
        selections_->RemoveSet(index);
        if (selections_->GetNumberOfSets() == 0) {
            // You can remove the last set, but there must always be one
            // set, so we re-create one. (So removing the last set has the
            // effect of clearing it.)
            selections_->NewSet();
        }
        UpdateSelectionSetList();
    }

    void SelectSelectionSet(int index) {
        settings.selection_sets->SetSelectedIndex(index);
        selections_->SelectSet(index);
    }

    void UpdateSelectionSetList() {
        size_t n = selections_->GetNumberOfSets();
        int idx = settings.selection_sets->GetSelectedIndex();
        idx = std::max(0, idx);
        idx = std::min(idx, int(n) - 1);

        std::vector<std::string> items;
        items.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            std::stringstream s;
            s << "Set " << (i + 1);
            items.push_back(s.str());
        }
        settings.selection_sets->SetItems(items);
        SelectSelectionSet(idx);
        window_->PostRedraw();
    }

    void UpdateSelectablePoints() {
        selections_->StartSelectablePoints();
        for (auto &o : objects_) {
            if (!IsGeometryVisible(o)) {
                continue;
            }

            selections_->AddSelectablePoints(o.name, o.geometry.get(),
                                             o.tgeometry.get());
        }
        selections_->EndSelectablePoints();
    }

    bool OnAnimationTick() {
        auto now = Application::GetInstance().Now();
        if (now >= next_animation_tick_clock_time_) {
            SetCurrentFrame(ui_state_.current_frame + 1);
            UpdateAnimationTickClockTime(now);

            return true;
        }
        return false;
    }

    void UpdateAnimationTickClockTime(double now) {
        next_animation_tick_clock_time_ = now + ui_state_.frame_delay;
    }

    void ExportCurrentImage(const std::string &path) {
        scene_->EnableSceneCaching(false);
        scene_->GetScene()->GetScene()->RenderToImage(
                [this, path](std::shared_ptr<geometry::Image> image) mutable {
                    if (!io::WriteImage(path, *image)) {
                        this->window_->ShowMessageBox(
                                "Error",
                                (std::string("Could not write image to ") +
                                 path + ".")
                                        .c_str());
                    }
                    scene_->EnableSceneCaching(true);
                });
    }

    void OnAbout() {
        auto &theme = window_->GetTheme();
        auto dlg = std::make_shared<gui::Dialog>("About");

        auto title = std::make_shared<gui::Label>(
                (std::string("Open3D ") + OPEN3D_VERSION).c_str());
        auto text = std::make_shared<gui::Label>(
                "The MIT License (MIT)\n"
                "Copyright (c) 2018 - 2020 www.open3d.org\n\n"

                "Permission is hereby granted, free of charge, to any person "
                "obtaining a copy of this software and associated "
                "documentation "
                "files (the \"Software\"), to deal in the Software without "
                "restriction, including without limitation the rights to use, "
                "copy, modify, merge, publish, distribute, sublicense, and/or "
                "sell copies of the Software, and to permit persons to whom "
                "the Software is furnished to do so, subject to the following "
                "conditions:\n\n"

                "The above copyright notice and this permission notice shall "
                "be "
                "included in all copies or substantial portions of the "
                "Software.\n\n"

                "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY "
                "KIND, "
                "EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE "
                "WARRANTIES "
                "OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND "
                "NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT "
                "HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, "
                "WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING "
                "FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR "
                "OTHER DEALINGS IN THE SOFTWARE.");
        auto ok = std::make_shared<gui::Button>("OK");
        ok->SetOnClicked([this]() { this->window_->CloseDialog(); });

        gui::Margins margins(theme.font_size);
        auto layout = std::make_shared<gui::Vert>(0, margins);
        layout->AddChild(gui::Horiz::MakeCentered(title));
        layout->AddFixed(theme.font_size);
        layout->AddChild(text);
        layout->AddFixed(theme.font_size);
        layout->AddChild(gui::Horiz::MakeCentered(ok));
        dlg->AddChild(layout);

        window_->ShowDialog(dlg);
    }

    void OnExportRGB() {
        auto dlg = std::make_shared<gui::FileDialog>(
                gui::FileDialog::Mode::SAVE, "Save File", window_->GetTheme());
        dlg->AddFilter(".png", "PNG images (.png)");
        dlg->AddFilter("", "All files");
        dlg->SetOnCancel([this]() { this->window_->CloseDialog(); });
        dlg->SetOnDone([this](const char *path) {
            this->window_->CloseDialog();
            this->ExportCurrentImage(path);
        });
        window_->ShowDialog(dlg);
    }

    void OnClose() { window_->Close(); }

    void OnToggleSettings() {
        bool is_checked = !ui_state_.show_settings;
        ui_state_.show_settings = is_checked;
        settings.panel->SetVisible(is_checked);
        Application::GetInstance().GetMenubar()->SetChecked(MENU_SETTINGS,
                                                            is_checked);
        window_->SetNeedsLayout();
    }

    std::string UniquifyName(const std::string &name) {
        if (added_names_.find(name) == added_names_.end()) {
            return name;
        }

        int n = 0;
        std::string unique;
        do {
            n += 1;
            std::stringstream s;
            s << name << "_" << n;
            unique = s.str();
        } while (added_names_.find(unique) != added_names_.end());

        return unique;
    }

    Eigen::Vector4f CalcDefaultUnlitColor() {
        float luminosity = 0.21f * ui_state_.bg_color.x() +
                           0.72f * ui_state_.bg_color.y() +
                           0.07f * ui_state_.bg_color.z();
        if (luminosity >= 0.5f) {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        } else {
            return {1.0f, 1.0f, 1.0f, 1.0f};
        }
    }

    std::vector<std::string> GetListOfIBLs() {
        std::vector<std::string> ibls;
        std::vector<std::string> resource_files;
        std::string resource_path =
                Application::GetInstance().GetResourcePath();
        utility::filesystem::ListFilesInDirectory(resource_path,
                                                  resource_files);
        std::sort(resource_files.begin(), resource_files.end());
        for (auto &f : resource_files) {
            if (f.find("_ibl.ktx") == f.size() - 8) {
                auto name = utility::filesystem::GetFileNameWithoutDirectory(f);
                name = name.substr(0, name.size() - 8);
                ibls.push_back(name);
            }
        }
        return ibls;
    }
};

// ----------------------------------------------------------------------------
O3DVisualizer::O3DVisualizer(const std::string &title, int width, int height)
    : Window(title, width, height), impl_(new O3DVisualizer::Impl()) {
    impl_->Construct(this);

    // Create the app menu. We will take over the existing menubar (if any)
    // since a) we need to cache a pointer, and b) we should be the only
    // window, since the whole point of this class is to have an easy way to
    // visualize something with a blocking call to draw().
    auto menu = std::make_shared<Menu>();
#if defined(__APPLE__)
    // The first menu item to be added on macOS becomes the application
    // menu (no matter its name)
    auto app_menu = std::make_shared<Menu>();
    app_menu->AddItem("About", MENU_ABOUT);
    menu->AddMenu("Open3D", app_menu);
#endif  // __APPLE__
    auto file_menu = std::make_shared<Menu>();
    file_menu->AddItem("Export Current Image...", MENU_EXPORT_RGB);
    file_menu->AddSeparator();
    file_menu->AddItem("Close Window", MENU_CLOSE, KeyName::KEY_W);
    menu->AddMenu("File", file_menu);

    auto actions_menu = std::make_shared<Menu>();
    actions_menu->AddItem("Show Settings", MENU_SETTINGS);
    actions_menu->SetChecked(MENU_SETTINGS, false);
    menu->AddMenu("Actions", actions_menu);
    impl_->settings.actions_menu = actions_menu.get();

#if !defined(__APPLE__)
    auto help_menu = std::make_shared<Menu>();
    help_menu->AddItem("About", MENU_ABOUT);
    menu->AddMenu("Help", help_menu);
#endif  // !__APPLE__

    Application::GetInstance().SetMenubar(menu);

    SetOnMenuItemActivated(MENU_ABOUT, [this]() { this->impl_->OnAbout(); });
    SetOnMenuItemActivated(MENU_EXPORT_RGB,
                           [this]() { this->impl_->OnExportRGB(); });
    SetOnMenuItemActivated(MENU_CLOSE, [this]() { this->impl_->OnClose(); });
    SetOnMenuItemActivated(MENU_SETTINGS,
                           [this]() { this->impl_->OnToggleSettings(); });

    impl_->ui_state_.show_settings = true;  // opposite, we will toggle
    impl_->OnToggleSettings();  // must do this after menu is created
}

O3DVisualizer::~O3DVisualizer() {}

Open3DScene *O3DVisualizer::GetScene() const {
    return impl_->scene_->GetScene().get();
}

void O3DVisualizer::AddAction(const std::string &name,
                              std::function<void(O3DVisualizer &)> callback) {
    // Add button to the "Custom Actions" segment in the UI
    SmallButton *button = new SmallButton(name.c_str());
    button->SetOnClicked([this, callback]() { callback(*this); });
    impl_->settings.actions->AddChild(GiveOwnership(button));

    SetNeedsLayout();
    impl_->settings.actions_panel->SetVisible(true);
    impl_->settings.actions_panel->SetIsOpen(true);

    if (impl_->can_auto_show_settings_ &&
        impl_->settings.actions->size() == 1) {
        ShowSettings(true);
    }

    // Add menu item
    if (impl_->settings.menuid2action.empty()) {
        impl_->settings.actions_menu->AddSeparator();
    }
    int id = MENU_ACTIONS_BASE + int(impl_->settings.menuid2action.size());
    impl_->settings.actions_menu->AddItem(name.c_str(), id);
    impl_->settings.menuid2action[id] = callback;
    SetOnMenuItemActivated(id, [this, callback]() { callback(*this); });
}

void O3DVisualizer::SetBackgroundColor(const Eigen::Vector4f &bg_color) {
    impl_->SetBackgroundColor(bg_color);
}

void O3DVisualizer::SetShader(Shader shader) { impl_->SetShader(shader); }

void O3DVisualizer::AddGeometry(const std::string &name,
                                std::shared_ptr<geometry::Geometry3D> geom,
                                rendering::Material *material /*= nullptr*/,
                                const std::string &group /*= ""*/,
                                double time /*= 0.0*/,
                                bool is_visible /*= true*/) {
    impl_->AddGeometry(name, geom, nullptr, material, group, time, is_visible);
}

void O3DVisualizer::AddGeometry(const std::string &name,
                                std::shared_ptr<t::geometry::Geometry> tgeom,
                                rendering::Material *material /*= nullptr*/,
                                const std::string &group /*= ""*/,
                                double time /*= 0.0*/,
                                bool is_visible /*= true*/) {
    impl_->AddGeometry(name, nullptr, tgeom, material, group, time, is_visible);
}

void O3DVisualizer::RemoveGeometry(const std::string &name) {
    return impl_->RemoveGeometry(name);
}

void O3DVisualizer::ShowGeometry(const std::string &name, bool show) {
    return impl_->ShowGeometry(name, show);
}

O3DVisualizer::DrawObject O3DVisualizer::GetGeometry(
        const std::string &name) const {
    return impl_->GetGeometry(name);
}

void O3DVisualizer::ShowSettings(bool show) { impl_->ShowSettings(show); }

void O3DVisualizer::ShowSkybox(bool show) { impl_->ShowSkybox(show); }

void O3DVisualizer::ShowAxes(bool show) { impl_->ShowAxes(show); }

void O3DVisualizer::SetPointSize(int point_size) {
    impl_->SetPointSize(point_size);
}

void O3DVisualizer::EnableGroup(const std::string &group, bool enable) {
    impl_->EnableGroup(group, enable);
}

std::vector<O3DVisualizerSelections::SelectionSet>
O3DVisualizer::GetSelectionSets() const {
    return impl_->GetSelectionSets();
}

double O3DVisualizer::GetAnimationFrameDelay() const {
    return impl_->ui_state_.frame_delay;
}

void O3DVisualizer::SetAnimationFrameDelay(double secs) {
    impl_->ui_state_.frame_delay = secs;
}

size_t O3DVisualizer::GetCurrentFrame() const {
    return impl_->ui_state_.current_frame;
}

void O3DVisualizer::SetCurrentFrame(size_t f) { impl_->SetCurrentFrame(f); }

bool O3DVisualizer::GetIsAnimating() const {
    return impl_->ui_state_.is_animating;
}

void O3DVisualizer::SetAnimating(bool is_animating) {
    impl_->SetAnimating(is_animating);
}

void O3DVisualizer::SetupCamera(float fov,
                                const Eigen::Vector3f &center,
                                const Eigen::Vector3f &eye,
                                const Eigen::Vector3f &up) {}

void O3DVisualizer::ResetCameraToDefault() {
    return impl_->ResetCameraToDefault();
}

O3DVisualizer::UIState O3DVisualizer::GetUIState() const {
    return impl_->ui_state_;
}

void O3DVisualizer::ExportCurrentImage(const std::string &path) {
    impl_->ExportCurrentImage(path);
}

void O3DVisualizer::Layout(const Theme &theme) {
    auto em = theme.font_size;
    int settings_width = 15 * theme.font_size;
#if !GROUPS_USE_TREE
    if (impl_->added_groups_.size() >= 2) {
        settings_width += 5 * theme.font_size;
    }
#endif  // !GROUPS_USE_TREE
    if (impl_->frames_.GetNumberOfFrames() > 1) {
        settings_width += 3 * theme.font_size;
    }

    auto f = GetContentRect();
    impl_->settings.actions->SetWidth(settings_width -
                                      int(std::round(1.5 * em)));
    if (impl_->settings.panel->IsVisible()) {
        impl_->scene_->SetFrame(
                Rect(f.x, f.y, f.width - settings_width, f.height));
        impl_->settings.panel->SetFrame(Rect(f.GetRight() - settings_width, f.y,
                                             settings_width, f.height));
    } else {
        impl_->scene_->SetFrame(f);
    }

    Super::Layout(theme);
}

}  // namespace visualizer
}  // namespace visualization
}  // namespace open3d