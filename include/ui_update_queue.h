// SPDX-License-Identifier: GPL-3.0-or-later
// Thread-safe UI update queue for LVGL
//
// Any thread can queue lambdas via anneal::ui::queue_update().
// A high-priority LVGL timer drains the queue at the start of each
// lv_timer_handler() cycle, BEFORE rendering. This guarantees widget
// mutations never happen mid-render.
//
// Tagged callbacks: queue_update("tag", cb) stores a debug tag that the
// crash handler writes to crash.txt if the process crashes mid-callback.

#pragma once

#include "crash_handler.h"

#include <lvgl.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

namespace anneal::ui {

using UpdateCallback = std::function<void()>;

struct TaggedCallback {
    const char* tag = nullptr;
    UpdateCallback callback;
};

class UpdateQueue {
  public:
    static UpdateQueue& instance() {
        static UpdateQueue inst;
        return inst;
    }

    void init() {
        if (initialized_) return;
        shut_down_ = false;
        timer_ = lv_timer_create(timer_cb, 1, this);
        if (!timer_) {
            spdlog::error("[UpdateQueue] Failed to create timer");
            return;
        }

        // Register tag pointers with crash handler
        crash_handler::register_callback_tag_ptr(&current_tag_);
        crash_handler::register_previous_tag_ring(
            previous_tag_ring_, kPreviousTagRingSize, &previous_tag_next_);

        initialized_ = true;
        spdlog::debug("[UpdateQueue] Initialized");
    }

    void queue(UpdateCallback callback) {
        queue(nullptr, std::move(callback));
    }

    void queue(const char* tag, UpdateCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shut_down_) return;
        pending_.push({tag, std::move(callback)});
    }

    void shutdown() {
        process_pending();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initialized_ = false;
            shut_down_ = true;
            std::queue<TaggedCallback>().swap(pending_);
        }
        timer_ = nullptr;
    }

    void process_pending() {
        std::queue<TaggedCallback> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty()) return;
            batch.swap(pending_);
        }
        while (!batch.empty()) {
            current_tag_ = batch.front().tag;
            try {
                batch.front().callback();
            } catch (const std::exception& e) {
                spdlog::error("[UpdateQueue] Callback exception: {}", e.what());
            }
            // Record completed tag in ring
            if (current_tag_) {
                unsigned int idx = previous_tag_next_ % kPreviousTagRingSize;
                previous_tag_ring_[idx] = current_tag_;
                previous_tag_next_++;
            }
            current_tag_ = nullptr;
            batch.pop();
        }
    }

  private:
    UpdateQueue() = default;

    static void timer_cb(lv_timer_t* timer) {
        auto* self = static_cast<UpdateQueue*>(lv_timer_get_user_data(timer));
        if (self) self->process_pending();
    }

    std::mutex mutex_;
    std::queue<TaggedCallback> pending_;
    lv_timer_t* timer_ = nullptr;
    bool initialized_ = false;
    std::atomic<bool> shut_down_{false};

    // Crash diagnostics: current + recent callback tags
    static inline volatile const char* current_tag_ = nullptr;
    static constexpr unsigned int kPreviousTagRingSize = 4;
    static inline volatile const char* previous_tag_ring_[kPreviousTagRingSize] = {};
    static inline volatile unsigned int previous_tag_next_ = 0;
};

// ── Free functions ──────────────────────────────────────────────────────

inline void queue_update(UpdateCallback callback) {
    UpdateQueue::instance().queue(std::move(callback));
}

inline void update_queue_init() {
    UpdateQueue::instance().init();
}

inline void update_queue_shutdown() {
    UpdateQueue::instance().shutdown();
}

} // namespace anneal::ui
