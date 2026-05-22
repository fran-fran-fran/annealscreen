// SPDX-License-Identifier: GPL-3.0-or-later
// Temperature graph widget for AnnealScreen.
// Follows HelixScreen's ui_temp_graph.cpp pattern:
// - lv_chart with LV_CHART_TYPE_LINE, LV_CHART_UPDATE_MODE_SHIFT
// - Deci-degrees (x10) for 0.1C precision
// - Custom draw callbacks for grid, Y-axis, X-axis, target lines, legend
// - Static label buffers for deferred LVGL draw
// - Feature flags for toggling visual elements

#include "ui_temp_graph.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ── Helpers ─────────────────────────────────────────────────────────────

static anneal_series_meta_t* find_series(anneal_temp_graph_t* graph, int id) {
    if (!graph) return nullptr;
    for (int i = 0; i < ANNEAL_GRAPH_MAX_SERIES; ++i) {
        if (graph->series_meta[i].chart_series && graph->series_meta[i].id == id)
            return &graph->series_meta[i];
    }
    return nullptr;
}

// Mute a color toward the background (for target lines)
static lv_color_t mute_color(lv_color_t color, lv_opa_t opa, lv_color_t bg) {
    return lv_color_mix(color, bg, opa);
}

// ── Draw callbacks ──────────────────────────────────────────────────────

// Grid lines: horizontal + vertical divisions constrained to content area
static void draw_grid_lines_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* graph = static_cast<anneal_temp_graph_t*>(lv_event_get_user_data(e));
    if (!layer || !graph) return;
    if (!(graph->features & ANNEAL_GRAPH_GRID)) return;

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_top    = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    int32_t pad_left   = lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    int32_t pad_right  = lv_obj_get_style_pad_right(chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);

    int32_t cx1 = coords.x1 + pad_left;
    int32_t cx2 = coords.x2 - pad_right;
    int32_t cy1 = coords.y1 + pad_top;
    int32_t cy2 = coords.y2 - pad_bottom;
    int32_t cw = cx2 - cx1;
    int32_t ch = cy2 - cy1;
    if (cw <= 0 || ch <= 0) return;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = graph->cached_grid_color;
    line_dsc.width = 1;
    line_dsc.opa = LV_OPA_30;

    // Horizontal grid lines (5 divisions)
    constexpr int H_DIV = 5;
    for (int i = 0; i <= H_DIV; ++i) {
        int32_t y = cy1 + (ch * i) / H_DIV;
        line_dsc.p1 = {cx1, y};
        line_dsc.p2 = {cx2, y};
        lv_draw_line(layer, &line_dsc);
    }

    // Vertical grid lines (10 divisions)
    constexpr int V_DIV = 10;
    for (int i = 0; i <= V_DIV; ++i) {
        int32_t x = cx1 + (cw * i) / V_DIV;
        line_dsc.p1 = {x, cy1};
        line_dsc.p2 = {x, cy2};
        lv_draw_line(layer, &line_dsc);
    }
}

// Y-axis temperature labels (right-aligned in left padding area)
static void draw_y_axis_labels_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* graph = static_cast<anneal_temp_graph_t*>(lv_event_get_user_data(e));
    if (!layer || !graph || !graph->show_y_axis || graph->y_axis_increment <= 0)
        return;

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_top    = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);
    int32_t content_top = coords.y1 + pad_top;
    int32_t content_bottom = coords.y2 - pad_bottom;
    int32_t content_height = content_bottom - content_top;
    if (content_height <= 0) return;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = graph->cached_text_color;
    label_dsc.font = graph->axis_font;
    label_dsc.align = LV_TEXT_ALIGN_RIGHT;

    int32_t label_height = lv_font_get_line_height(graph->axis_font);
    float temp_range = graph->max_temp - graph->min_temp;
    if (temp_range <= 0) return;

    // Static buffer ring for deferred LVGL draw (HelixScreen pattern)
    static char y_bufs[8][8];
    static int y_idx = 0;
    y_idx = 0;

    for (float temp = graph->min_temp; temp <= graph->max_temp;
         temp += graph->y_axis_increment) {
        float frac = (graph->max_temp - temp) / temp_range;
        int32_t label_y = content_top + static_cast<int32_t>(frac * content_height);
        label_y -= label_height / 2;
        if (label_y < coords.y1) label_y = coords.y1;
        if (label_y + label_height > coords.y2) continue;

        char* buf = y_bufs[y_idx++ % 8];
        std::snprintf(buf, 8, "%d\xC2\xB0", static_cast<int>(temp));

        lv_area_t label_area = {
            coords.x1, label_y,
            coords.x1 + graph->y_axis_width, label_y + label_height
        };
        label_dsc.text = buf;
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// X-axis elapsed time labels (centered below content area)
static void draw_x_axis_labels_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* graph = static_cast<anneal_temp_graph_t*>(lv_event_get_user_data(e));
    if (!layer || !graph || !graph->show_x_axis) return;

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_left   = lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    int32_t pad_right  = lv_obj_get_style_pad_right(chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);
    int32_t content_x1 = coords.x1 + pad_left;
    int32_t content_x2 = coords.x2 - pad_right;
    int32_t content_width = content_x2 - content_x1;
    if (content_width <= 0) return;

    int32_t label_height = lv_font_get_line_height(graph->axis_font);
    int32_t label_y = coords.y2 - pad_bottom + 4;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = graph->cached_text_color;
    label_dsc.font = graph->axis_font;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;

    // Determine total time span from point count (1 sample/sec)
    float total_time_s = static_cast<float>(graph->point_count);

    // Choose label interval based on total time
    float interval_s;
    if (total_time_s < 120)        interval_s = 30;
    else if (total_time_s < 600)   interval_s = 60;
    else if (total_time_s < 1800)  interval_s = 300;
    else if (total_time_s < 7200)  interval_s = 600;
    else                           interval_s = 1800;

    // Elapsed time at left and right edges
    float right_s = graph->elapsed_latest_s;
    float left_s = right_s - total_time_s;
    if (left_s < 0) left_s = 0;

    float first_label_s = std::ceil(left_s / interval_s) * interval_s;

    static char x_bufs[8][16];
    static int x_idx = 0;
    x_idx = 0;

    for (float t = first_label_s; t <= right_s && x_idx < 8; t += interval_s) {
        float frac = (t - left_s) / total_time_s;
        if (frac < 0 || frac > 1) continue;
        int32_t label_x = content_x1 + static_cast<int32_t>(frac * content_width);

        int mins = static_cast<int>(t / 60.0f);
        char* buf = x_bufs[x_idx++ % 8];
        if (mins >= 60)
            std::snprintf(buf, 16, "%dh%dm", mins / 60, mins % 60);
        else
            std::snprintf(buf, 16, "%dm", mins);

        lv_area_t label_area = {
            label_x - 25, label_y,
            label_x + 25, label_y + label_height
        };
        label_dsc.text = buf;
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// Target temperature dashed lines
static void draw_target_lines_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    auto* graph = static_cast<anneal_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph || !graph->chart) return;
    if (!(graph->features & ANNEAL_GRAPH_TARGET_LINES)) return;

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_left   = lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    int32_t pad_right  = lv_obj_get_style_pad_right(chart, LV_PART_MAIN);
    int32_t pad_top    = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);

    int32_t cx1 = coords.x1 + pad_left;
    int32_t cx2 = coords.x2 - pad_right;
    int32_t cy1 = coords.y1 + pad_top;
    int32_t cy2 = coords.y2 - pad_bottom;
    int32_t ch = cy2 - cy1;
    if (ch <= 0) return;

    for (int i = 0; i < ANNEAL_GRAPH_MAX_SERIES; ++i) {
        auto* meta = &graph->series_meta[i];
        if (!meta->chart_series || !meta->visible || !meta->show_target)
            continue;

        int32_t content_y = ch - lv_map(
            static_cast<int32_t>(meta->target_temp * ANNEAL_TEMP_SCALE),
            static_cast<int32_t>(graph->min_temp * ANNEAL_TEMP_SCALE),
            static_cast<int32_t>(graph->max_temp * ANNEAL_TEMP_SCALE),
            0, ch);
        int32_t abs_y = cy1 + content_y;
        if (abs_y < cy1 || abs_y > cy2) continue;

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = mute_color(meta->color, LV_OPA_50, graph->cached_bg_color);
        line_dsc.width = 1;
        line_dsc.dash_width = 6;
        line_dsc.dash_gap = 4;
        line_dsc.p1 = {cx1, abs_y};
        line_dsc.p2 = {cx2, abs_y};
        lv_draw_line(layer, &line_dsc);
    }
}

// Legend chips (color swatch + name) in upper-left corner
static void draw_legend_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    auto* graph = static_cast<anneal_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph || !(graph->features & ANNEAL_GRAPH_LEGEND)) return;

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_left = lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    int32_t pad_top  = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);

    int32_t x = coords.x1 + pad_left + 6;
    int32_t y = coords.y1 + pad_top + 4;
    int32_t swatch_size = 8;
    int32_t chip_gap = 10;

    static char legend_bufs[ANNEAL_GRAPH_MAX_SERIES][32];
    int buf_idx = 0;

    for (int i = 0; i < ANNEAL_GRAPH_MAX_SERIES && buf_idx < ANNEAL_GRAPH_MAX_SERIES; ++i) {
        auto* meta = &graph->series_meta[i];
        if (!meta->chart_series || !meta->visible) continue;

        std::strncpy(legend_bufs[buf_idx], meta->name, 31);
        legend_bufs[buf_idx][31] = '\0';

        // Color swatch
        lv_draw_rect_dsc_t swatch_dsc;
        lv_draw_rect_dsc_init(&swatch_dsc);
        swatch_dsc.bg_color = meta->color;
        swatch_dsc.bg_opa = LV_OPA_COVER;
        swatch_dsc.radius = LV_RADIUS_CIRCLE;
        lv_area_t swatch_area = {x, y + 2, x + swatch_size, y + 2 + swatch_size};
        lv_draw_rect(layer, &swatch_dsc, &swatch_area);

        // Label
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = graph->cached_text_color;
        label_dsc.font = graph->axis_font;
        label_dsc.opa = LV_OPA_80;
        label_dsc.text = legend_bufs[buf_idx];
        lv_area_t label_area = {
            x + swatch_size + 3, y,
            x + swatch_size + 3 + 80, y + lv_font_get_line_height(graph->axis_font)
        };
        lv_draw_label(layer, &label_dsc, &label_area);

        x += swatch_size + 3 + 80 + chip_gap;
        buf_idx++;
    }
}

// Chart delete callback — null out chart pointer
static void chart_delete_cb(lv_event_t* e) {
    auto* graph = static_cast<anneal_temp_graph_t*>(lv_event_get_user_data(e));
    if (graph) graph->chart = nullptr;
}

// ── Core API ────────────────────────────────────────────────────────────

anneal_temp_graph_t* anneal_temp_graph_create(lv_obj_t* parent) {
    if (!parent) return nullptr;

    auto* graph = new anneal_temp_graph_t();
    std::memset(graph, 0, sizeof(anneal_temp_graph_t));

    graph->point_count = ANNEAL_GRAPH_DEFAULT_POINTS;
    graph->min_temp = ANNEAL_GRAPH_DEFAULT_MIN;
    graph->max_temp = ANNEAL_GRAPH_DEFAULT_MAX;
    graph->features = ANNEAL_GRAPH_ALL_FEATURES;
    graph->y_axis_increment = 50.0f;
    graph->show_y_axis = true;
    graph->show_x_axis = true;
    graph->axis_font = lv_font_get_default();
    graph->y_axis_width = 40;

    // Cache theme colors
    auto& theme = anneal::ThemeManager::instance();
    graph->cached_grid_color = theme.has_color("elevated_bg")
        ? theme.get_color("elevated_bg") : lv_color_hex(0x3A3A52);
    graph->cached_bg_color = theme.has_color("overlay_bg")
        ? theme.get_color("overlay_bg") : lv_color_hex(0x0F0F1A);
    graph->cached_text_color = theme.has_color("text_muted")
        ? theme.get_color("text_muted") : lv_color_hex(0xA8A8B8);

    // Create lv_chart
    graph->chart = lv_chart_create(parent);
    if (!graph->chart) {
        spdlog::error("[TempGraph] Failed to create chart");
        delete graph;
        return nullptr;
    }

    lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(graph->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(graph->chart, static_cast<uint32_t>(graph->point_count));

    // Y-axis range in deci-degrees
    lv_chart_set_axis_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y,
                            static_cast<int32_t>(graph->min_temp * ANNEAL_TEMP_SCALE),
                            static_cast<int32_t>(graph->max_temp * ANNEAL_TEMP_SCALE));

    // Style: background
    lv_obj_set_style_bg_opa(graph->chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->chart, graph->cached_bg_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(graph->chart, 8, LV_PART_MAIN);

    // Padding: space for axis labels
    int32_t label_height = lv_font_get_line_height(graph->axis_font);
    lv_obj_set_style_pad_top(graph->chart, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(graph->chart, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_left(graph->chart, graph->y_axis_width + 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(graph->chart, 6 + label_height + 10, LV_PART_MAIN);

    // Series line style
    lv_obj_set_style_line_width(graph->chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_opa(graph->chart, LV_OPA_COVER, LV_PART_ITEMS);

    // Hide point indicators
    lv_obj_set_style_width(graph->chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(graph->chart, 0, LV_PART_INDICATOR);

    // Disable built-in division lines (we draw custom ones)
    lv_chart_set_div_line_count(graph->chart, 0, 0);

    // Register draw callbacks
    lv_obj_add_event_cb(graph->chart, draw_grid_lines_cb, LV_EVENT_DRAW_MAIN, graph);
    lv_obj_add_event_cb(graph->chart, draw_x_axis_labels_cb, LV_EVENT_DRAW_POST, graph);
    lv_obj_add_event_cb(graph->chart, draw_y_axis_labels_cb, LV_EVENT_DRAW_POST, graph);
    lv_obj_add_event_cb(graph->chart, draw_target_lines_cb, LV_EVENT_DRAW_POST, graph);
    lv_obj_add_event_cb(graph->chart, draw_legend_cb, LV_EVENT_DRAW_POST, graph);

    // Clean up graph pointer when chart widget is deleted
    lv_obj_add_event_cb(graph->chart, chart_delete_cb, LV_EVENT_DELETE, graph);

    // Store graph pointer for retrieval
    lv_obj_set_user_data(graph->chart, graph);

    // Text color for axis labels (match chart's text style)
    lv_obj_set_style_text_color(graph->chart, graph->cached_text_color, LV_PART_MAIN);

    spdlog::debug("[TempGraph] Created ({} points, range {}-{}\xC2\xB0)",
                  graph->point_count, (int)graph->min_temp, (int)graph->max_temp);
    return graph;
}

void anneal_temp_graph_destroy(anneal_temp_graph_t* graph) {
    if (!graph) return;
    if (graph->chart) {
        lv_obj_remove_event_cb(graph->chart, draw_grid_lines_cb);
        lv_obj_remove_event_cb(graph->chart, draw_x_axis_labels_cb);
        lv_obj_remove_event_cb(graph->chart, draw_y_axis_labels_cb);
        lv_obj_remove_event_cb(graph->chart, draw_target_lines_cb);
        lv_obj_remove_event_cb(graph->chart, draw_legend_cb);
        lv_obj_remove_event_cb(graph->chart, chart_delete_cb);
        lv_obj_delete(graph->chart);
        graph->chart = nullptr;
    }
    delete graph;
}

lv_obj_t* anneal_temp_graph_get_chart(anneal_temp_graph_t* graph) {
    return graph ? graph->chart : nullptr;
}

// ── Series management ───────────────────────────────────────────────────

int anneal_temp_graph_add_series(anneal_temp_graph_t* graph,
                                  const char* name, lv_color_t color) {
    if (!graph || !graph->chart) return -1;

    int slot = -1;
    for (int i = 0; i < ANNEAL_GRAPH_MAX_SERIES; ++i) {
        if (!graph->series_meta[i].chart_series) { slot = i; break; }
    }
    if (slot < 0) {
        spdlog::error("[TempGraph] No available series slots");
        return -1;
    }

    auto* ser = lv_chart_add_series(graph->chart, color, LV_CHART_AXIS_PRIMARY_Y);
    if (!ser) return -1;

    lv_chart_set_all_values(graph->chart, ser, LV_CHART_POINT_NONE);

    auto* meta = &graph->series_meta[slot];
    meta->id = graph->next_series_id++;
    meta->chart_series = ser;
    meta->color = color;
    std::strncpy(meta->name, name, sizeof(meta->name) - 1);
    meta->name[sizeof(meta->name) - 1] = '\0';
    meta->visible = true;
    meta->show_target = false;
    meta->target_temp = 0;
    meta->first_value_received = false;

    graph->series_count++;
    spdlog::debug("[TempGraph] Added series {} '{}' (slot {})", meta->id, name, slot);
    return meta->id;
}

void anneal_temp_graph_remove_series(anneal_temp_graph_t* graph, int series_id) {
    auto* meta = find_series(graph, series_id);
    if (!meta) return;
    lv_chart_remove_series(graph->chart, meta->chart_series);
    std::memset(meta, 0, sizeof(anneal_series_meta_t));
    graph->series_count--;
}

void anneal_temp_graph_show_series(anneal_temp_graph_t* graph,
                                    int series_id, bool visible) {
    auto* meta = find_series(graph, series_id);
    if (!meta) return;
    meta->visible = visible;
    lv_chart_hide_series(graph->chart, meta->chart_series, !visible);
    lv_obj_invalidate(graph->chart);
}

// ── Data updates ────────────────────────────────────────────────────────

void anneal_temp_graph_push_value(anneal_temp_graph_t* graph,
                                   int series_id, float temp) {
    auto* meta = find_series(graph, series_id);
    if (!meta) return;

    if (!meta->first_value_received) {
        meta->first_value_received = true;
        lv_chart_set_all_values(graph->chart, meta->chart_series,
                                static_cast<int32_t>(temp * ANNEAL_TEMP_SCALE));
    }

    lv_chart_set_next_value(graph->chart, meta->chart_series,
                            static_cast<int32_t>(temp * ANNEAL_TEMP_SCALE));
}

void anneal_temp_graph_push_value_with_time(anneal_temp_graph_t* graph,
                                             int series_id, float temp,
                                             float elapsed_s) {
    auto* meta = find_series(graph, series_id);
    if (!meta) return;

    if (!meta->first_value_received) {
        meta->first_value_received = true;
        lv_chart_set_all_values(graph->chart, meta->chart_series,
                                static_cast<int32_t>(temp * ANNEAL_TEMP_SCALE));
    }

    // Track elapsed time for X-axis labels
    if (graph->elapsed_origin_s == 0 && elapsed_s > 0)
        graph->elapsed_origin_s = elapsed_s;
    graph->elapsed_latest_s = elapsed_s;
    graph->visible_point_count++;

    lv_chart_set_next_value(graph->chart, meta->chart_series,
                            static_cast<int32_t>(temp * ANNEAL_TEMP_SCALE));
}

void anneal_temp_graph_set_series_data(anneal_temp_graph_t* graph,
                                        int series_id,
                                        const float* temps, int count) {
    auto* meta = find_series(graph, series_id);
    if (!meta || !temps || count <= 0) return;

    lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);

    int to_copy = std::min(count, graph->point_count);
    // If fewer points than capacity, right-align the data
    int offset = graph->point_count - to_copy;

    int32_t* y_data = lv_chart_get_y_array(graph->chart, meta->chart_series);
    if (!y_data) return;

    // Get start point to account for circular buffer
    uint32_t sp = lv_chart_get_x_start_point(graph->chart, meta->chart_series);
    for (int i = 0; i < to_copy; ++i) {
        int idx = (sp + offset + i) % graph->point_count;
        y_data[idx] = static_cast<int32_t>(temps[i] * ANNEAL_TEMP_SCALE);
    }

    meta->first_value_received = true;
    lv_chart_refresh(graph->chart);
}

void anneal_temp_graph_clear(anneal_temp_graph_t* graph) {
    if (!graph) return;
    for (int i = 0; i < ANNEAL_GRAPH_MAX_SERIES; ++i) {
        auto* meta = &graph->series_meta[i];
        if (meta->chart_series) {
            lv_chart_set_all_values(graph->chart, meta->chart_series,
                                    LV_CHART_POINT_NONE);
            meta->first_value_received = false;
        }
    }
    graph->elapsed_origin_s = 0;
    graph->elapsed_latest_s = 0;
    graph->visible_point_count = 0;
    lv_chart_refresh(graph->chart);
}

// ── Target temperature ──────────────────────────────────────────────────

void anneal_temp_graph_set_series_target(anneal_temp_graph_t* graph,
                                          int series_id, float target,
                                          bool show) {
    auto* meta = find_series(graph, series_id);
    if (!meta) return;
    meta->target_temp = target;
    meta->show_target = show;
    if (graph->chart) lv_obj_invalidate(graph->chart);
}

// ── Configuration ───────────────────────────────────────────────────────

void anneal_temp_graph_set_temp_range(anneal_temp_graph_t* graph,
                                       float min_temp, float max_temp) {
    if (!graph || !graph->chart) return;
    graph->min_temp = min_temp;
    graph->max_temp = max_temp;
    lv_chart_set_axis_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y,
                            static_cast<int32_t>(min_temp * ANNEAL_TEMP_SCALE),
                            static_cast<int32_t>(max_temp * ANNEAL_TEMP_SCALE));
}

void anneal_temp_graph_set_point_count(anneal_temp_graph_t* graph, int count) {
    if (!graph || !graph->chart || count <= 0) return;
    graph->point_count = count;
    lv_chart_set_point_count(graph->chart, static_cast<uint32_t>(count));
}

void anneal_temp_graph_set_y_axis(anneal_temp_graph_t* graph,
                                   float increment, bool show) {
    if (!graph) return;
    graph->y_axis_increment = increment;
    graph->show_y_axis = show;
    if (graph->chart) lv_obj_invalidate(graph->chart);
}

void anneal_temp_graph_set_features(anneal_temp_graph_t* graph,
                                     uint32_t features) {
    if (!graph) return;
    graph->features = features | ANNEAL_GRAPH_LINES; // Lines always on
    graph->show_y_axis = (features & ANNEAL_GRAPH_Y_AXIS) != 0;
    graph->show_x_axis = (features & ANNEAL_GRAPH_X_AXIS) != 0;
    if (graph->chart) lv_obj_invalidate(graph->chart);
}

uint32_t anneal_temp_graph_get_features(anneal_temp_graph_t* graph) {
    return graph ? graph->features : 0;
}