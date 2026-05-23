// SPDX-License-Identifier: GPL-3.0-or-later

#include "home_panel.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "ui_update_queue.h"

#include "helix-xml/helix_xml.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Forward declaration — defined in main.cpp (global namespace)
void show_setup_overlay();

// MDI icon font (compiled from assets/fonts/mdi_icons_24.c, C linkage)
extern "C" { LV_FONT_DECLARE(mdi_icons_24); }

namespace anneal {

// Chart series colors — sourced from theme tokens so XML and C++ stay in sync
static lv_color_t get_planned_color() { return ui_theme_get_color("chart_planned"); }
static lv_color_t get_actual_color() { return ui_theme_get_color("chart_actual"); }

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

    lv_xml_register_event_cb(nullptr, "on_annealr_start", HomePanel::on_start_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_pause", HomePanel::on_pause_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_resume", HomePanel::on_resume_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_cancel", HomePanel::on_cancel_clicked);
    lv_xml_register_event_cb(nullptr, "on_annealr_settings", HomePanel::on_settings_clicked);

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

    StaticPanelRegistry::instance().register_destroy(
        "HomePanel", []() {
            auto& p = HomePanel::instance();
            if (p.graph_) {
                anneal_temp_graph_destroy(p.graph_);
                p.graph_ = nullptr;
            }
            if (p.chart_timer_) {
                lv_timer_delete(p.chart_timer_);
                p.chart_timer_ = nullptr;
            }
        });
}

lv_obj_t* HomePanel::create(lv_obj_t* parent) {
    panel_ = static_cast<lv_obj_t*>(
        lv_xml_create(parent, "home_panel", nullptr));
    if (!panel_) {
        spdlog::error("[HomePanel] Failed to create XML component");
        return nullptr;
    }

    profile_list_ = lv_obj_find_by_name(panel_, "annealr_profile_list");
    chart_container_ = lv_obj_find_by_name(panel_, "annealr_chart_container");

    // Set MDI icon font on settings button (cog = U+F0493)
    lv_obj_t* settings_icon = lv_obj_find_by_name(panel_, "btn_settings_icon");
    if (settings_icon) {
        lv_obj_set_style_text_font(settings_icon, &mdi_icons_24, LV_PART_MAIN);
        lv_label_set_text(settings_icon, "\xF3\xB0\x92\x93"); // MDI cog
    }

    // Create temperature graph inside the container
    if (chart_container_) {
        graph_ = anneal_temp_graph_create(chart_container_);
        if (graph_) {
            lv_obj_t* chart = anneal_temp_graph_get_chart(graph_);
            if (chart) {
                lv_obj_set_size(chart, LV_PCT(100), LV_PCT(100));
            }

            // Add series: target setpoint curve + actual temperature
            planned_series_ = anneal_temp_graph_add_series(
                graph_, "Target", get_planned_color());
            actual_series_ = anneal_temp_graph_add_series(
                graph_, "Actual", get_actual_color());

            // Target series: dashed line + horizontal marker at latest value
            anneal_temp_graph_set_series_dashed(graph_, planned_series_, true);
            anneal_temp_graph_set_series_h_marker(graph_, planned_series_, true);

            // Configure Y-axis: 50-degree increments
            anneal_temp_graph_set_y_axis(graph_, 50.0f, true);

            spdlog::info("[HomePanel] Chart created with {} series",
                         graph_->series_count);
        }
    }

    populate_profile_list();
    setup_observers();

    chart_timer_ = lv_timer_create(HomePanel::timer_cb, CHART_UPDATE_MS, this);

    return panel_;
}

// ── Observers ───────────────────────────────────────────────────────────

void HomePanel::setup_observers() {
    observers_.emplace_back(anneal::ui::observe_string<HomePanel>(
        state_.state_subject(), this,
        [](HomePanel* self, const char*) {
            self->update_button_states();
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
        lv_obj_t* btn = lv_button_create(profile_list_);
        if (!btn) continue;

        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);

        lv_obj_set_user_data(btn, const_cast<void*>(
            static_cast<const void*>(&profile)));

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

                // Load planned profile curve into chart
                self->load_planned_profile(*prof, 22.0f);
            }
        }, LV_EVENT_CLICKED, this);
    }

    if (profiles.empty()) {
        lv_obj_t* placeholder = lv_label_create(profile_list_);
        lv_label_set_text(placeholder, "No profiles found");
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

    spdlog::debug("[HomePanel] Button update: state='{}' start={} pause={} "
                  "resume={} cancel={}",
                  state_.current_state_str(), can_start, is_running,
                  is_paused, can_cancel);

    lv_subject_set_int(&can_start_,  can_start  ? 1 : 0);
    lv_subject_set_int(&can_pause_,  is_running ? 1 : 0);
    lv_subject_set_int(&can_resume_, is_paused  ? 1 : 0);
    lv_subject_set_int(&can_cancel_, can_cancel ? 1 : 0);
}

// ── Chart management ────────────────────────────────────────────────────

void HomePanel::push_temperature(float temp_c, float elapsed_s) {
    if (!graph_ || actual_series_ < 0) return;
    anneal_temp_graph_push_value_with_time(graph_, actual_series_,
                                            temp_c, elapsed_s);
}

void HomePanel::push_target_setpoint(float target_c, float elapsed_s) {
    if (!graph_ || planned_series_ < 0 || target_c <= 0) return;
    anneal_temp_graph_push_value_with_time(graph_, planned_series_,
                                            target_c, elapsed_s);
}

void HomePanel::load_planned_profile(const AnnealrProfile& profile,
                                      float start_temp) {
    if (!graph_ || planned_series_ < 0) return;

    auto points = profile.build_planned_curve(start_temp);
    if (points.empty()) return;

    // Find temp range for Y-axis
    float t_min = 1e9f, t_max = -1e9f;
    for (const auto& [t, temp] : points) {
        t_min = std::min(t_min, temp);
        t_max = std::max(t_max, temp);
    }
    t_min = std::max(0.0f, t_min - 10.0f);
    t_max += 20.0f;

    // Round to nice values
    t_min = std::floor(t_min / 50.0f) * 50.0f;
    t_max = std::ceil(t_max / 50.0f) * 50.0f;
    if (t_max < 100.0f) t_max = 100.0f;

    anneal_temp_graph_set_temp_range(graph_, t_min, t_max);

    // Only change point count when no run is active — changing it resets all series
    float total_s = points.back().first;
    if (total_s <= 0) total_s = 600;

    if (!state_.is_run_active()) {
        int point_count = std::max(300, std::min(1200, static_cast<int>(total_s)));
        anneal_temp_graph_set_point_count(graph_, point_count);
    }

    // Clear only the planned series, preserve actual temperature data
    auto* planned_meta = &graph_->series_meta[planned_series_];
    if (planned_meta->chart_series) {
        lv_chart_set_all_values(graph_->chart, planned_meta->chart_series,
                                LV_CHART_POINT_NONE);
        planned_meta->first_value_received = false;
    }

    // Interpolate planned curve to match point count
    int pc = graph_->point_count;
    std::vector<float> temps(pc);
    for (int i = 0; i < pc; ++i) {
        float t = (static_cast<float>(i) / (pc - 1)) * total_s;

        // Find surrounding points
        size_t j = 0;
        while (j + 1 < points.size() && points[j + 1].first < t) ++j;

        if (j + 1 >= points.size()) {
            temps[i] = points.back().second;
        } else {
            float t0 = points[j].first, t1 = points[j + 1].first;
            float v0 = points[j].second, v1 = points[j + 1].second;
            float frac = (t1 > t0) ? (t - t0) / (t1 - t0) : 0;
            temps[i] = v0 + frac * (v1 - v0);
        }
    }

    anneal_temp_graph_set_series_data(graph_, planned_series_,
                                       temps.data(), pc);

    // Set elapsed time range for X-axis
    graph_->elapsed_latest_s = total_s;
    graph_->elapsed_origin_s = 0;
}

void HomePanel::clear_chart() {
    if (!graph_) return;
    anneal_temp_graph_clear(graph_);
}

void HomePanel::update_chart_target(float target_c) {
    if (!graph_ || actual_series_ < 0) return;
    anneal_temp_graph_set_series_target(graph_, actual_series_,
                                         target_c, target_c > 0);
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

    // Clear actual series data for fresh run
    if (self.graph_ && self.actual_series_ >= 0) {
        auto* meta = &self.graph_->series_meta[self.actual_series_];
        if (meta->chart_series) {
            lv_chart_set_all_values(self.graph_->chart, meta->chart_series,
                                    LV_CHART_POINT_NONE);
            meta->first_value_received = false;
        }
        self.graph_->elapsed_origin_s = 0;
        self.graph_->elapsed_latest_s = 0;
        self.graph_->visible_point_count = 0;
    }

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

void HomePanel::on_settings_clicked(lv_event_t*) {
    spdlog::info("[HomePanel] Opening settings");
    ::show_setup_overlay();
}

// ── Chart timer ─────────────────────────────────────────────────────────

void HomePanel::timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<HomePanel*>(lv_timer_get_user_data(timer));
    if (!self || !self->graph_) return;
    // Invalidate chart periodically for smooth updates
    if (anneal_temp_graph_is_valid(self->graph_))
        lv_obj_invalidate(self->graph_->chart);
}

} // namespace anneal