// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ui_temp_graph.h
 * @brief Temperature chart widget wrapping lv_chart for annealing profiles
 *
 * Follows HelixScreen's ui_temp_graph pattern:
 * - Struct-based C API wrapping lv_chart
 * - Feature flags for toggling chart elements
 * - Draw callbacks for axis labels, grid, target lines
 * - Deci-degrees (x10) for 0.1C precision in lv_chart
 * - Static label buffers for deferred LVGL draw
 *
 * Usage:
 * @code
 *   anneal_temp_graph_t* g = anneal_temp_graph_create(parent);
 *   int planned = anneal_temp_graph_add_series(g, "Planned", lv_color_hex(0x66B2FF));
 *   int actual  = anneal_temp_graph_add_series(g, "Actual",  lv_color_hex(0xFF7326));
 *   anneal_temp_graph_set_series_target(g, actual, 140.0f, true);
 *
 *   // Push mode: one value at a time
 *   anneal_temp_graph_push_value(g, actual, current_temp);
 *
 *   // Array mode: load entire planned curve
 *   anneal_temp_graph_set_series_data(g, planned, temps, count);
 *
 *   anneal_temp_graph_destroy(g);
 * @endcode
 */

#pragma once

#include <lvgl.h>
#include <cstdint>

// ── Configuration ───────────────────────────────────────────────────────

#define ANNEAL_GRAPH_MAX_SERIES     4
#define ANNEAL_GRAPH_DEFAULT_POINTS 600   // 10 minutes at 1 sample/sec
#define ANNEAL_GRAPH_DEFAULT_MIN    0.0f
#define ANNEAL_GRAPH_DEFAULT_MAX    200.0f

// Internal scale: deci-degrees (x10) for 0.1C precision in lv_chart int32
static constexpr int32_t ANNEAL_TEMP_SCALE = 10;

// ── Feature flags ───────────────────────────────────────────────────────

enum anneal_graph_feature {
    ANNEAL_GRAPH_LINES        = (1 << 0),
    ANNEAL_GRAPH_TARGET_LINES = (1 << 1),
    ANNEAL_GRAPH_Y_AXIS       = (1 << 2),
    ANNEAL_GRAPH_X_AXIS       = (1 << 3),
    ANNEAL_GRAPH_GRID         = (1 << 4),
    ANNEAL_GRAPH_LEGEND       = (1 << 5),
};

#define ANNEAL_GRAPH_ALL_FEATURES \
    (ANNEAL_GRAPH_LINES | ANNEAL_GRAPH_TARGET_LINES | \
     ANNEAL_GRAPH_Y_AXIS | ANNEAL_GRAPH_X_AXIS | \
     ANNEAL_GRAPH_GRID | ANNEAL_GRAPH_LEGEND)

// ── Series metadata ─────────────────────────────────────────────────────

struct anneal_series_meta_t {
    int id;
    lv_chart_series_t* chart_series;
    lv_color_t color;
    char name[32];
    bool visible;
    bool dashed;            // draw as dashed line
    bool show_target;
    float target_temp;
    bool show_h_marker;     // horizontal dashed line at latest value
    bool first_value_received;
    float latest_value;     // last pushed value (for h_marker)
};

// ── Graph struct ────────────────────────────────────────────────────────

struct anneal_temp_graph_t {
    lv_obj_t* chart;
    anneal_series_meta_t series_meta[ANNEAL_GRAPH_MAX_SERIES];
    int series_count;
    int next_series_id;
    int point_count;
    float min_temp;
    float max_temp;

    uint32_t features;
    float y_axis_increment;
    bool show_y_axis;
    bool show_x_axis;

    const lv_font_t* axis_font;
    int32_t y_axis_width;

    // Elapsed time tracking for X-axis
    float elapsed_origin_s;    // elapsed_s at first push
    float elapsed_latest_s;    // elapsed_s at most recent push
    int visible_point_count;

    // Cached theme colors (set once at creation, avoid per-frame lookups)
    lv_color_t cached_grid_color;
    lv_color_t cached_bg_color;
    lv_color_t cached_text_color;
};

// ── Core API ────────────────────────────────────────────────────────────

anneal_temp_graph_t* anneal_temp_graph_create(lv_obj_t* parent);
void anneal_temp_graph_destroy(anneal_temp_graph_t* graph);
lv_obj_t* anneal_temp_graph_get_chart(anneal_temp_graph_t* graph);

static inline bool anneal_temp_graph_is_valid(anneal_temp_graph_t* graph) {
    return graph && graph->chart;
}

// ── Series management ───────────────────────────────────────────────────

int anneal_temp_graph_add_series(anneal_temp_graph_t* graph,
                                 const char* name, lv_color_t color);
void anneal_temp_graph_remove_series(anneal_temp_graph_t* graph, int series_id);
void anneal_temp_graph_show_series(anneal_temp_graph_t* graph,
                                   int series_id, bool visible);

// ── Data updates ────────────────────────────────────────────────────────

/// Push a single value (shift mode — scrolls old data left)
void anneal_temp_graph_push_value(anneal_temp_graph_t* graph,
                                   int series_id, float temp);

/// Push with elapsed time (for X-axis labels)
void anneal_temp_graph_push_value_with_time(anneal_temp_graph_t* graph,
                                             int series_id, float temp,
                                             float elapsed_s);

/// Load entire array (for planned profile curve)
void anneal_temp_graph_set_series_data(anneal_temp_graph_t* graph,
                                        int series_id,
                                        const float* temps, int count);

/// Clear all series data
void anneal_temp_graph_clear(anneal_temp_graph_t* graph);

// ── Target temperature ──────────────────────────────────────────────────

void anneal_temp_graph_set_series_target(anneal_temp_graph_t* graph,
                                          int series_id, float target,
                                          bool show);
void anneal_temp_graph_set_series_dashed(anneal_temp_graph_t* graph,
                                          int series_id, bool dashed);
void anneal_temp_graph_set_series_h_marker(anneal_temp_graph_t* graph,
                                            int series_id, bool show);

// ── Configuration ───────────────────────────────────────────────────────

void anneal_temp_graph_set_temp_range(anneal_temp_graph_t* graph,
                                       float min_temp, float max_temp);
void anneal_temp_graph_set_point_count(anneal_temp_graph_t* graph, int count);
void anneal_temp_graph_set_y_axis(anneal_temp_graph_t* graph,
                                   float increment, bool show);
void anneal_temp_graph_set_features(anneal_temp_graph_t* graph,
                                     uint32_t features);
uint32_t anneal_temp_graph_get_features(anneal_temp_graph_t* graph);