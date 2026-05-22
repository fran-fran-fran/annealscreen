// SPDX-License-Identifier: GPL-3.0-or-later
// Main panel for AnnealScreen: profile list, controls, temperature chart.
//
// Single panel application. Layout defined in ui_xml/home_panel.xml.
// Uses anneal_temp_graph_t for chart (lv_chart under the hood).

#pragma once

#include "annealr_state.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "ui_observer_guard.h"
#include "ui_temp_graph.h"

#include <lvgl.h>

#include <string>
#include <vector>

namespace anneal {

class HomePanel {
  public:
    static HomePanel& instance();

    void init_subjects();
    lv_obj_t* create(lv_obj_t* parent);
    void set_client(MoonrakerClient* client) { client_ = client; }

    void populate_profile_list();
    void update_button_states();

    // Chart management — called from main.cpp status handler
    void push_temperature(float temp_c, float elapsed_s);
    void push_target_setpoint(float target_c, float elapsed_s);
    void load_planned_profile(const AnnealrProfile& profile, float start_temp);
    void clear_chart();
    void update_chart_target(float target_c);

    // Access the graph for external configuration
    anneal_temp_graph_t* graph() const { return graph_; }

  private:
    HomePanel();

    void setup_observers();
    void send_gcode(const char* command);

    static void on_start_clicked(lv_event_t* e);
    static void on_pause_clicked(lv_event_t* e);
    static void on_resume_clicked(lv_event_t* e);
    static void on_cancel_clicked(lv_event_t* e);
    static void on_settings_clicked(lv_event_t* e);

    static void timer_cb(lv_timer_t* timer);

    MoonrakerClient* client_ = nullptr;
    AnnealrState& state_;

    lv_obj_t* panel_        = nullptr;
    lv_obj_t* profile_list_ = nullptr;
    lv_obj_t* chart_container_ = nullptr;

    // Temperature graph (lv_chart wrapper)
    anneal_temp_graph_t* graph_ = nullptr;
    int planned_series_ = -1;
    int actual_series_  = -1;

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
};

} // namespace anneal