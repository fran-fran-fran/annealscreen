// SPDX-License-Identifier: GPL-3.0-or-later
// Domain state for the annealr Klipper plugin.
//
// Owns LVGL subjects for run state, profile info, stage progress, timing,
// and chamber temperature. Updated from Moonraker WebSocket status
// notifications via queue_update() for thread safety.

#pragma once

#include <lvgl.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace anneal {

// ── Profile data model ──────────────────────────────────────────────────

struct AnnealrSegment {
    std::string kind;       // "ramp" or "soak"
    float       target;     // target temperature (°C)
    float       rate;       // ramp rate (°C/min), 0 if unset
    float       duration_s; // explicit duration (seconds), 0 if unset
};

struct AnnealrProfile {
    std::string name;
    std::string description;
    std::vector<AnnealrSegment> segments;

    float estimate_duration_s(float start_temp = 22.0f) const;
    std::vector<std::pair<float, float>> build_planned_curve(
        float start_temp = 22.0f) const;
};

// ── Temperature sample for live chart ───────────────────────────────────

struct TempSample {
    float elapsed_s;
    float temp_c;
};

// ── Singleton state ─────────────────────────────────────────────────────

class AnnealrState {
  public:
    static AnnealrState& instance();

    // Lifecycle — call from main thread
    void init_subjects();
    void deinit_subjects();

    // Profile loading — call from main thread after config query
    void load_profiles_from_config(const std::string& config_json);

    // Status update — call from WebSocket thread (defers internally)
    void update_from_status(const std::string& status_json);

    // Temperature recording — call from WebSocket thread (defers internally)
    void record_temperature(float temp_c, float run_elapsed_s);

    // ── Subject accessors ───────────────────────────────────────────

    lv_subject_t* state_subject()          { return &state_; }
    lv_subject_t* profile_name_subject()   { return &profile_name_; }
    lv_subject_t* stage_label_subject()    { return &stage_label_; }
    lv_subject_t* stage_index_subject()    { return &stage_index_; }
    lv_subject_t* stage_count_subject()    { return &stage_count_; }
    lv_subject_t* stage_target_subject()   { return &stage_target_; }
    lv_subject_t* progress_subject()       { return &progress_; }
    lv_subject_t* elapsed_s_subject()      { return &elapsed_s_; }
    lv_subject_t* remaining_s_subject()    { return &remaining_s_; }
    lv_subject_t* run_elapsed_s_subject()  { return &run_elapsed_s_; }
    lv_subject_t* status_text_subject()    { return &status_text_; }
    lv_subject_t* profiles_version_subject() { return &profiles_version_; }
    lv_subject_t* chamber_temp_subject()   { return &chamber_temp_; }
    lv_subject_t* chamber_target_subject() { return &chamber_target_; }
    lv_subject_t* chamber_temp_text_subject() { return &chamber_temp_text_; }

    // ── Profile access ──────────────────────────────────────────────

    const std::vector<AnnealrProfile>& profiles() const { return profiles_; }

    // ── Temperature history for chart ───────────────────────────────

    const std::deque<TempSample>& temp_history() const { return temp_history_; }
    void clear_temp_history();

    // ── Convenience queries ─────────────────────────────────────────

    const char* current_state_str() const { return state_buf_; }
    bool is_run_active() const;
    bool is_paused() const;
    bool can_start() const;

  private:
    AnnealrState() = default;

    void apply_status(const std::string& state_str,
                      const std::string& profile_name,
                      int stage_index, int stage_count,
                      const std::string& stage_label,
                      float stage_target, float progress,
                      float elapsed_s, float remaining_s,
                      float run_elapsed_s);

    void update_status_text(const std::string& state_str,
                            const std::string& profile_name,
                            const std::string& stage_label,
                            float progress, float remaining_s,
                            float run_elapsed_s);

    static AnnealrSegment parse_segment_line(const std::string& line,
                                              float prev_target);

    bool initialized_ = false;

    // LVGL subjects
    lv_subject_t state_{};
    lv_subject_t profile_name_{};
    lv_subject_t stage_label_{};
    lv_subject_t stage_index_{};
    lv_subject_t stage_count_{};
    lv_subject_t stage_target_{};
    lv_subject_t progress_{};
    lv_subject_t elapsed_s_{};
    lv_subject_t remaining_s_{};
    lv_subject_t run_elapsed_s_{};
    lv_subject_t status_text_{};
    lv_subject_t profiles_version_{};
    lv_subject_t chamber_temp_{};     // centidegrees (int) - for chart
    lv_subject_t chamber_target_{};   // centidegrees (int)
    lv_subject_t chamber_temp_text_{}; // formatted "28.6°C" for display

    // Static buffers for string subjects
    char state_buf_[32]        = "idle";
    char profile_name_buf_[64] = "";
    char stage_label_buf_[128] = "";
    char status_text_buf_[512] = "";
    char chamber_temp_text_buf_[32] = "";

    // Profile storage
    std::vector<AnnealrProfile> profiles_;
    std::mutex profiles_mutex_;

    // Temperature history
    std::deque<TempSample> temp_history_;
    std::mutex temp_mutex_;
    static constexpr size_t MAX_TEMP_SAMPLES = 2000;
};

} // namespace anneal