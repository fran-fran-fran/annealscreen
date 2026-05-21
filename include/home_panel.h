// SPDX-License-Identifier: GPL-3.0-or-later
// Main panel for AnnealScreen: profile list, controls, temperature chart.
//
// This is the only panel in the application. No navigation stack needed.
// Layout is defined in ui_xml/home-panel.xml; this class owns panel-local
// subjects, wires observers, handles events, and draws the chart.

#pragma once

#include "annealr_state.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "ui_observer_guard.h"

#include <lvgl.h>

#include <string>
#include <vector>

namespace anneal {

class HomePanel {
  public:
    static HomePanel& instance();

    // Lifecycle
    void init_subjects();
    lv_obj_t* create(lv_obj_t* parent);
    void set_client(MoonrakerClient* client) { client_ = client; }

    // Profile list management
    void populate_profile_list();
    void update_button_states();

  private:
    HomePanel();

    void setup_observers();
    void send_gcode(const char* command);

    // Event callbacks (registered globally for XML)
    static void on_start_clicked(lv_event_t* e);
    static void on_pause_clicked(lv_event_t* e);
    static void on_resume_clicked(lv_event_t* e);
    static void on_cancel_clicked(lv_event_t* e);

    // Chart rendering
    static void chart_draw_cb(lv_event_t* e);
    void draw_chart(lv_event_t* e);
    void draw_profile_preview(lv_layer_t* layer, lv_area_t* area);
    void draw_live_chart(lv_layer_t* layer, lv_area_t* area);
    void draw_grid(lv_layer_t* layer, lv_area_t* area,
                   float time_min, float time_max,
                   float temp_min, float temp_max);
    void draw_polyline(lv_layer_t* layer, lv_area_t* area,
                       const std::vector<std::pair<float, float>>& points,
                       float time_max, float temp_min, float temp_max,
                       lv_color_t color, lv_opa_t opa, int width);

    static float nice_step(float range, int target_ticks);
    static float nice_time_step(float range_s, int target_ticks);

    // Timer callback for periodic chart refresh
    static void timer_cb(lv_timer_t* timer);

    MoonrakerClient* client_ = nullptr;
    AnnealrState& state_;

    lv_obj_t* panel_        = nullptr;
    lv_obj_t* profile_list_ = nullptr;
    lv_obj_t* chart_canvas_ = nullptr;
    lv_timer_t* chart_timer_ = nullptr;

    std::vector<ObserverGuard> observers_;

    // Panel-local subjects
    lv_subject_t selected_profile_{};
    char selected_profile_buf_[64] = "";

    lv_subject_t can_start_{};
    lv_subject_t can_pause_{};
    lv_subject_t can_resume_{};
    lv_subject_t can_cancel_{};

    bool subjects_initialized_ = false;

    static constexpr uint32_t CHART_UPDATE_MS = 2000;
    static constexpr int MARGIN_LEFT   = 45;
    static constexpr int MARGIN_RIGHT  = 10;
    static constexpr int MARGIN_TOP    = 15;
    static constexpr int MARGIN_BOTTOM = 30;
};

} // namespace anneal
