// SPDX-License-Identifier: GPL-3.0-or-later
// Factory functions for type-safe LVGL observers with RAII cleanup.
//
// Provides observe_int_sync() and observe_string() — the two patterns
// AnnealScreen needs. Both defer callbacks via queue_update() to prevent
// re-entrant observer destruction crashes.
//
// Stripped from HelixScreen's observer_factory.h: no printer_state.h,
// no moonraker_client.h, no ConnectionState/PrintJobState helpers.

#pragma once

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include <lvgl.h>

#include <memory>
#include <string>
#include <type_traits>

namespace anneal::ui {

namespace detail {

template <typename Panel, typename Handler>
struct LambdaObserverContext {
    Panel* panel;
    Handler handler;
    // Weak alive token: when the ObserverGuard is destroyed, its cleanup
    // callback deletes this context, which destroys the shared_ptr and
    // expires any weak_ptrs held by deferred lambdas.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace detail

/// Create a deferred int observer. The handler runs on the main thread
/// via queue_update() after the current subject notification completes.
///
/// @tparam Panel   Class type (used for the this pointer)
/// @tparam Handler Callable: void(Panel*, int32_t)
template <typename Panel, typename Handler>
ObserverGuard observe_int_sync(lv_subject_t* subject, Panel* panel,
                                Handler&& handler) {
    if (!subject || !panel) return ObserverGuard();

    using H = std::decay_t<Handler>;
    auto* ctx = new detail::LambdaObserverContext<Panel, H>{
        panel, std::forward<Handler>(handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::LambdaObserverContext<Panel, H>*>(
                lv_observer_get_user_data(obs));
            if (!c || !c->panel) return;
            int32_t value = lv_subject_get_int(subj);
            auto handler_copy = c->handler;
            auto* panel_ptr = c->panel;
            std::weak_ptr<bool> weak_alive = c->alive;
            anneal::ui::queue_update([handler_copy, panel_ptr, value, weak_alive]() {
                if (weak_alive.expired()) return;
                handler_copy(panel_ptr, value);
            });
        },
        ctx, [ctx]() { delete ctx; });
    return guard;
}

/// Create a deferred string observer. The handler runs on the main thread.
/// The string value is copied to ensure it remains valid across the defer.
///
/// @tparam Panel   Class type
/// @tparam Handler Callable: void(Panel*, const char*)
template <typename Panel, typename Handler>
ObserverGuard observe_string(lv_subject_t* subject, Panel* panel,
                              Handler&& handler) {
    if (!subject || !panel) return ObserverGuard();

    using H = std::decay_t<Handler>;
    auto* ctx = new detail::LambdaObserverContext<Panel, H>{
        panel, std::forward<Handler>(handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::LambdaObserverContext<Panel, H>*>(
                lv_observer_get_user_data(obs));
            if (!c || !c->panel) return;
            const char* str = lv_subject_get_string(subj);
            std::string str_copy = str ? str : "";
            auto handler_copy = c->handler;
            auto* panel_ptr = c->panel;
            std::weak_ptr<bool> weak_alive = c->alive;
            anneal::ui::queue_update([handler_copy, panel_ptr, str_copy, weak_alive]() {
                if (weak_alive.expired()) return;
                handler_copy(panel_ptr, str_copy.c_str());
            });
        },
        ctx, [ctx]() { delete ctx; });
    return guard;
}

} // namespace anneal::ui
