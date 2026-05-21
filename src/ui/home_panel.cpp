// SPDX-License-Identifier: GPL-3.0-or-later

#include "home_panel.h"
#include "static_subject_registry.h"
#include "ui_update_queue.h"

#include "helix-xml/helix_xml.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace anneal {

// ── Chart colors (hardcoded — not themed, chart is custom-drawn) ────────

static const lv_color_t COLOR_PLANNED    = lv_color_make(0x66, 0xB2, 0xFF);
static const lv_color_t COLOR_ACTUAL     = lv_color_make(0xFF, 0x73, 0x26);
static const lv_color_t COLOR_GRID       = lv_color_make(0x40, 0x40, 0x47);
static const lv_color_t COLOR_GRID_TEXT  = lv_color_make(0x80, 0x80, 0x8C);
static const lv_color_t COLOR_MARKER     = lv_color_make(0x33, 0xFF, 0x66);
static const lv_color_t COLOR_TARGET     = lv_color_make(0xFF, 0xFF, 0x4D);

// ── Singleton ───────────────────────────────────────────────────────────

HomePanel& HomePanel::instance() {
    static HomePanel inst;
    return inst;
}

HomePanel::HomePanel() : state_(AnnealrState::instance()) {}

// ── Lifecycle ───────────────────────────────────────────────────────────

void HomePanel::init_subjects() {
    if (subjects_initialized_) return;
    spdlog::info("[HomePanel] Initializing subjects");

    std::memset(selected_profile_buf_, 0, sizeof(selected_profile_buf_));
    lv_subject_init_string(&selected_profile_, selected_profile_buf_, nullptr,
                           sizeof(selected_profile_buf_), "");

    lv_subject_init_int(&can_start_, 0);
    lv_subject_init_int(&can_pause_, 0);
    lv_subject_init_int(&can_resume_, 0);
    lv_subject_init_int(&can_cancel_, 0);

    lv_xml_register_subject(nullptr, "annealr_selected_profile", &selected_profile_);
    lv_xml_register_subject(nullptr, "annealr_can_start", &can_start_);
    lv_xml_register_subject(nullptr, "annealr_can_pause", &can_pause_);
    lv_xml_register_subject(nullptr, "annealr_can_resume", &can_resume_);
    lv_xml_register_subject(nullptr, "annealr_can_cancel", &can_cancel_);

    // Register event callbacks BEFORE XML creation
    lv_xml_register_event_cb(nullptr, "on_annealr_start", HomePanel::on_start_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_pause", HomePanel::on_pause_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_resume", HomePanel::on_resume_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_cancel", HomePanel::on_cancel_clicked);

    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "HomePanel", []() {
            auto& p = HomePanel::instance();
            if (p.subjects_initialized_) {
                lv_subject_deinit(&p.selected_profile_);
                lv_subject_deinit(&p.can_start_);
                lv_subject_deinit(&p.can_pause_);
                lv_subject_deinit(&p.can_resume_);
                lv_subject_deinit(&p.can_cancel_);
                p.subjects_initialized_ = false;
            }
        });
}

lv_obj_t* HomePanel::create(lv_obj_t* parent) {
    panel_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "home_panel", nullptr));
    if (!panel_) {
        spdlog::error("[HomePanel] Failed to create XML component");
        return nullptr;
    }

    // Find named widgets from the XML tree
    profile_list_ = lv_obj_find_by_name(panel_, "annealr_profile_list");
    chart_canvas_ = lv_obj_find_by_name(panel_, "annealr_chart");

    // Wire chart draw callback
    if (chart_canvas_) {
        lv_obj_add_event_cb(chart_canvas_, HomePanel::chart_draw_cb,
                            LV_EVENT_DRAW_MAIN, this);
    }

    populate_profile_list();
    setup_observers();

    // Start chart refresh timer
    chart_timer_ = lv_timer_create(HomePanel::timer_cb, CHART_UPDATE_MS, this);

    return panel_;
}

// ── Observers ───────────────────────────────────────────────────────────

void HomePanel::setup_observers() {
    observers_.emplace_back(anneal::ui::observe_string<HomePanel>(
        state_.state_subject(), this,
        [](HomePanel* self, const char*) {
            self->update_button_states();
            if (self->chart_canvas_) lv_obj_invalidate(self->chart_canvas_);
        }));

    observers_.emplace_back(anneal::ui::observe_int_sync<HomePanel>(
        state_.progress_subject(), this,
        [](HomePanel* self, int32_t) {
            if (self->chart_canvas_) lv_obj_invalidate(self->chart_canvas_);
        }));

    observers_.emplace_back(anneal::ui::observe_int_sync<HomePanel>(
        state_.profiles_version_subject(), this,
        [](HomePanel* self, int32_t) {
            self->populate_profile_list();
        }));
}

// ── Profile list ────────────────────────────────────────────────────────

void HomePanel::populate_profile_list() {
    if (!profile_list_) return;
    lv_obj_clean(profile_list_);

    const auto& profiles = state_.profiles();

    for (const auto& profile : profiles) {
        // Create a button manually (profile_list_ is a plain lv_obj container,
        // not lv_list — helix-xml has no lv_list parser)
        lv_obj_t* btn = lv_button_create(profile_list_);
        if (!btn) continue;

        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D2D4A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);

        lv_obj_set_user_data(btn, const_cast<void*>(
            static_cast<const void*>(&profile)));

        // Build display text: name + description + estimated duration
        char text[256];
        float dur = profile.estimate_duration_s();
        if (!profile.description.empty() && dur > 0) {
            int mins = static_cast<int>(dur / 60.0f);
            int hours = mins / 60;
            int rem   = mins % 60;
            if (hours > 0) {
                std::snprintf(text, sizeof(text), "%s\n%s (~%dh %dm)",
                              profile.name.c_str(),
                              profile.description.c_str(), hours, rem);
            } else {
                std::snprintf(text, sizeof(text), "%s\n%s (~%dm)",
                              profile.name.c_str(),
                              profile.description.c_str(), mins);
            }
        } else if (!profile.description.empty()) {
            std::snprintf(text, sizeof(text), "%s\n%s",
                          profile.name.c_str(),
                          profile.description.c_str());
        } else {
            std::snprintf(text, sizeof(text), "%s", profile.name.c_str());
        }

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        // Profile selection handler — using lv_obj_add_event_cb here because
        // these are dynamically created list items, not XML-declared widgets.
        // This is an acceptable exception to the "no lv_obj_add_event_cb" rule.
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<HomePanel*>(lv_event_get_user_data(e));
            auto* clicked_btn = lv_event_get_target_obj(e);
            auto* prof = static_cast<const AnnealrProfile*>(
                lv_obj_get_user_data(clicked_btn));
            if (prof && self) {
                std::strncpy(self->selected_profile_buf_, prof->name.c_str(),
                             sizeof(self->selected_profile_buf_) - 1);
                self->selected_profile_buf_[
                    sizeof(self->selected_profile_buf_) - 1] = '\0';
                lv_subject_copy_string(&self->selected_profile_,
                                       self->selected_profile_buf_);
                self->update_button_states();
                if (self->chart_canvas_)
                    lv_obj_invalidate(self->chart_canvas_);
            }
        }, LV_EVENT_CLICKED, this);
    }

    if (profiles.empty()) {
        lv_obj_t* placeholder = lv_label_create(profile_list_);
        lv_label_set_text(placeholder, "No profiles found");
        lv_obj_set_style_text_color(placeholder, lv_color_hex(0x808080),
                                    LV_PART_MAIN);
        lv_obj_set_width(placeholder, lv_pct(100));
        lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
    }
}

// ── Button states ───────────────────────────────────────────────────────

void HomePanel::update_button_states() {
    bool has_selection = (selected_profile_buf_[0] != '\0');
    bool can_start     = state_.can_start() && has_selection;
    bool is_running    = state_.is_run_active() && !state_.is_paused();
    bool is_paused     = state_.is_paused();
    bool can_cancel    = state_.is_run_active();

    lv_subject_set_int(&can_start_,  can_start  ? 1 : 0);
    lv_subject_set_int(&can_pause_,  is_running ? 1 : 0);
    lv_subject_set_int(&can_resume_, is_paused  ? 1 : 0);
    lv_subject_set_int(&can_cancel_, can_cancel ? 1 : 0);
}

// ── GCode dispatch ──────────────────────────────────────────────────────

void HomePanel::send_gcode(const char* command) {
    if (!client_) {
        spdlog::warn("[HomePanel] No Moonraker client, cannot send: {}", command);
        return;
    }
    spdlog::info("[HomePanel] Sending: {}", command);
    client_->execute_gcode(command);
}

void HomePanel::on_start_clicked(lv_event_t*) {
    auto& self = HomePanel::instance();
    if (self.selected_profile_buf_[0] == '\0') return;

    self.state_.clear_temp_history();
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "ANNEALR_START PROFILE=%s",
                  self.selected_profile_buf_);
    self.send_gcode(cmd);
}

void HomePanel::on_pause_clicked(lv_event_t*) {
    HomePanel::instance().send_gcode("ANNEALR_PAUSE");
}

void HomePanel::on_resume_clicked(lv_event_t*) {
    HomePanel::instance().send_gcode("ANNEALR_RESUME");
}

void HomePanel::on_cancel_clicked(lv_event_t*) {
    HomePanel::instance().send_gcode("ANNEALR_CANCEL");
}

// ── Chart timer ─────────────────────────────────────────────────────────

void HomePanel::timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<HomePanel*>(lv_timer_get_user_data(timer));
    if (!self || !self->chart_canvas_) return;
    if (self->state_.is_run_active())
        lv_obj_invalidate(self->chart_canvas_);
}

// ── Chart drawing ───────────────────────────────────────────────────────

void HomePanel::chart_draw_cb(lv_event_t* e) {
    auto* self = static_cast<HomePanel*>(lv_event_get_user_data(e));
    if (self) self->draw_chart(e);
}

void HomePanel::draw_chart(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    lv_area_t obj_area;
    lv_obj_get_coords(obj, &obj_area);

    int w = lv_area_get_width(&obj_area);
    int h = lv_area_get_height(&obj_area);
    if (w < 100 || h < 80) return;

    lv_area_t chart_area;
    chart_area.x1 = obj_area.x1 + MARGIN_LEFT;
    chart_area.y1 = obj_area.y1 + MARGIN_TOP;
    chart_area.x2 = obj_area.x2 - MARGIN_RIGHT;
    chart_area.y2 = obj_area.y2 - MARGIN_BOTTOM;

    if (state_.is_run_active()) {
        draw_live_chart(layer, &chart_area);
    } else if (selected_profile_buf_[0] != '\0') {
        draw_profile_preview(layer, &chart_area);
    } else {
        // Placeholder text
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = COLOR_GRID_TEXT;
        label_dsc.font = lv_font_default();
        label_dsc.text = "Select a profile";
        label_dsc.align = LV_TEXT_ALIGN_CENTER;

        lv_area_t text_area = chart_area;
        text_area.y1 = chart_area.y1 + (lv_area_get_height(&chart_area) / 2) - 10;
        lv_draw_label(layer, &label_dsc, &text_area);
    }
}

void HomePanel::draw_profile_preview(lv_layer_t* layer, lv_area_t* area) {
    const AnnealrProfile* profile = nullptr;
    for (const auto& p : state_.profiles()) {
        if (p.name == selected_profile_buf_) {
            profile = &p;
            break;
        }
    }
    if (!profile || profile->segments.empty()) return;

    auto points = profile->build_planned_curve(22.0f);
    if (points.empty()) return;

    float t_min = 1e9f, t_max = -1e9f;
    for (const auto& [t, temp] : points) {
        t_min = std::min(t_min, temp);
        t_max = std::max(t_max, temp);
    }
    t_min = std::max(0.0f, t_min - 10.0f);
    t_max += 10.0f;

    float time_max = points.back().first;
    if (time_max <= 0) time_max = 60.0f;

    draw_grid(layer, area, 0, time_max, t_min, t_max);
    draw_polyline(layer, area, points, time_max, t_min, t_max,
                  COLOR_PLANNED, LV_OPA_60, 3);
}

void HomePanel::draw_live_chart(lv_layer_t* layer, lv_area_t* area) {
    const char* active_name = lv_subject_get_string(
        state_.profile_name_subject());

    const AnnealrProfile* profile = nullptr;
    if (active_name && active_name[0] != '\0') {
        for (const auto& p : state_.profiles()) {
            if (p.name == active_name) {
                profile = &p;
                break;
            }
        }
    }

    float run_elapsed = static_cast<float>(
        lv_subject_get_int(state_.run_elapsed_s_subject()));

    std::vector<std::pair<float, float>> planned;
    if (profile) planned = profile->build_planned_curve(22.0f);

    float t_min = 1e9f, t_max = -1e9f;
    for (const auto& [t, temp] : planned) {
        t_min = std::min(t_min, temp);
        t_max = std::max(t_max, temp);
    }

    const auto& history = state_.temp_history();
    for (const auto& s : history) {
        t_min = std::min(t_min, s.temp_c);
        t_max = std::max(t_max, s.temp_c);
    }

    if (t_min > t_max) { t_min = 12.0f; t_max = 32.0f; }
    t_min = std::max(0.0f, t_min - 10.0f);
    t_max += 10.0f;

    float time_max = run_elapsed;
    if (!planned.empty())
        time_max = std::max(time_max, planned.back().first);
    time_max = std::max(time_max, 120.0f);
    time_max *= 1.05f;

    draw_grid(layer, area, 0, time_max, t_min, t_max);

    if (planned.size() >= 2)
        draw_polyline(layer, area, planned, time_max, t_min, t_max,
                      COLOR_PLANNED, LV_OPA_60, 2);

    if (!history.empty()) {
        std::vector<std::pair<float, float>> actual_pts;
        actual_pts.reserve(history.size());
        for (const auto& s : history)
            actual_pts.emplace_back(s.elapsed_s, s.temp_c);
        draw_polyline(layer, area, actual_pts, time_max, t_min, t_max,
                      COLOR_ACTUAL, LV_OPA_COVER, 3);
    }

    // Current position marker
    if (run_elapsed > 0) {
        int cw = lv_area_get_width(area);
        int px = area->x1 + static_cast<int>((run_elapsed / time_max) * cw);

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = COLOR_MARKER;
        line_dsc.opa = LV_OPA_90;
        line_dsc.width = 1;
        line_dsc.dash_width = 4;
        line_dsc.dash_gap = 4;
        line_dsc.p1.x = px; line_dsc.p1.y = area->y1;
        line_dsc.p2.x = px; line_dsc.p2.y = area->y2;
        lv_draw_line(layer, &line_dsc);
    }

    // Target temperature line
    int stage_target_centi = lv_subject_get_int(state_.stage_target_subject());
    if (stage_target_centi > 0) {
        float target = stage_target_centi / 10.0f;
        int ch = lv_area_get_height(area);
        float temp_range = t_max - t_min;
        if (temp_range > 0) {
            int py = area->y2 - static_cast<int>(
                ((target - t_min) / temp_range) * ch);

            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.color = COLOR_TARGET;
            line_dsc.opa = LV_OPA_30;
            line_dsc.width = 1;
            line_dsc.dash_width = 6;
            line_dsc.dash_gap = 4;
            line_dsc.p1.x = area->x1; line_dsc.p1.y = py;
            line_dsc.p2.x = area->x2; line_dsc.p2.y = py;
            lv_draw_line(layer, &line_dsc);
        }
    }
}

// ── Grid ────────────────────────────────────────────────────────────────

void HomePanel::draw_grid(lv_layer_t* layer, lv_area_t* area,
                           float time_min, float time_max,
                           float temp_min, float temp_max) {
    int cw = lv_area_get_width(area);
    int ch = lv_area_get_height(area);
    float temp_range = temp_max - temp_min;
    float time_range = time_max - time_min;
    if (temp_range <= 0 || time_range <= 0) return;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = COLOR_GRID;
    line_dsc.opa = LV_OPA_COVER;
    line_dsc.width = 1;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = COLOR_GRID_TEXT;
    label_dsc.font = lv_font_default();

    // Temperature grid (horizontal lines)
    float temp_step = nice_step(temp_range, 5);
    float t_tick = std::ceil(temp_min / temp_step) * temp_step;
    while (t_tick <= temp_max) {
        int py = area->y2 - static_cast<int>(
            ((t_tick - temp_min) / temp_range) * ch);

        line_dsc.p1.x = area->x1; line_dsc.p1.y = py;
        line_dsc.p2.x = area->x2; line_dsc.p2.y = py;
        lv_draw_line(layer, &line_dsc);

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d°C", static_cast<int>(t_tick));
        label_dsc.text = buf;

        lv_area_t label_area;
        label_area.x1 = area->x1 - MARGIN_LEFT + 2;
        label_area.y1 = py - 7;
        label_area.x2 = area->x1 - 4;
        label_area.y2 = py + 7;
        lv_draw_label(layer, &label_dsc, &label_area);

        t_tick += temp_step;
    }

    // Time grid (vertical lines)
    float time_step = nice_time_step(time_range, 6);
    float time_tick = std::ceil(time_min / time_step) * time_step;
    while (time_tick <= time_max) {
        int px = area->x1 + static_cast<int>(
            ((time_tick - time_min) / time_range) * cw);

        line_dsc.p1.x = px; line_dsc.p1.y = area->y1;
        line_dsc.p2.x = px; line_dsc.p2.y = area->y2;
        lv_draw_line(layer, &line_dsc);

        int mins = static_cast<int>(time_tick / 60.0f);
        char buf[16];
        if (mins >= 60)
            std::snprintf(buf, sizeof(buf), "%dh%dm", mins / 60, mins % 60);
        else
            std::snprintf(buf, sizeof(buf), "%dm", mins);

        label_dsc.text = buf;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;

        lv_area_t label_area;
        label_area.x1 = px - 20;
        label_area.y1 = area->y2 + 4;
        label_area.x2 = px + 20;
        label_area.y2 = area->y2 + MARGIN_BOTTOM - 2;
        lv_draw_label(layer, &label_dsc, &label_area);

        time_tick += time_step;
    }

    // Chart border
    lv_draw_rect_dsc_t border_dsc;
    lv_draw_rect_dsc_init(&border_dsc);
    border_dsc.bg_opa = LV_OPA_TRANSP;
    border_dsc.border_color = COLOR_GRID;
    border_dsc.border_width = 1;
    border_dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &border_dsc, area);
}

// ── Polyline ────────────────────────────────────────────────────────────

void HomePanel::draw_polyline(
        lv_layer_t* layer, lv_area_t* area,
        const std::vector<std::pair<float, float>>& points,
        float time_max, float temp_min, float temp_max,
        lv_color_t color, lv_opa_t opa, int width) {

    if (points.size() < 2) return;
    int cw = lv_area_get_width(area);
    int ch = lv_area_get_height(area);
    float temp_range = temp_max - temp_min;
    if (temp_range <= 0 || time_max <= 0) return;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.opa = opa;
    line_dsc.width = width;
    line_dsc.round_start = true;
    line_dsc.round_end = true;

    for (size_t i = 1; i < points.size(); ++i) {
        auto [t0, temp0] = points[i - 1];
        auto [t1, temp1] = points[i];

        line_dsc.p1.x = area->x1 + static_cast<int>((t0 / time_max) * cw);
        line_dsc.p1.y = area->y2 - static_cast<int>(
            ((temp0 - temp_min) / temp_range) * ch);
        line_dsc.p2.x = area->x1 + static_cast<int>((t1 / time_max) * cw);
        line_dsc.p2.y = area->y2 - static_cast<int>(
            ((temp1 - temp_min) / temp_range) * ch);

        lv_draw_line(layer, &line_dsc);
    }
}

// ── Axis helpers ────────────────────────────────────────────────────────

float HomePanel::nice_step(float range, int target_ticks) {
    float raw = range / std::max(target_ticks, 1);
    float magnitude = std::pow(10.0f, std::floor(std::log10(
        std::max(raw, 0.001f))));
    float residual = raw / magnitude;
    if (residual <= 1.5f)      return magnitude;
    else if (residual <= 3.5f) return 2.0f * magnitude;
    else if (residual <= 7.5f) return 5.0f * magnitude;
    return 10.0f * magnitude;
}

float HomePanel::nice_time_step(float range_s, int target_ticks) {
    float raw = range_s / std::max(target_ticks, 1);
    static const float candidates[] = {
        30, 60, 120, 300, 600, 900, 1200, 1800, 3600, 7200
    };
    for (float c : candidates) {
        if (c >= raw * 0.7f) return c;
    }
    return 7200.0f;
}

} // namespace anneal
