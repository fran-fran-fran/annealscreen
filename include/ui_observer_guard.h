// SPDX-License-Identifier: GPL-3.0-or-later
// RAII wrapper for LVGL observers — auto-removes on destruction.
//
// All AnnealScreen subjects are static singletons (no per-fan/per-sensor
// dynamic subjects), so SubjectLifetime tokens are not needed. This is a
// simplified version of HelixScreen's ObserverGuard.

#pragma once

#include <lvgl.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

class ObserverGuard {
  public:
    ObserverGuard() = default;

    ObserverGuard(lv_subject_t* subject, lv_observer_cb_t cb, void* user_data)
        : observer_(lv_subject_add_observer(subject, cb, user_data)) {}

    ObserverGuard(lv_subject_t* subject, lv_observer_cb_t cb, void* user_data,
                  std::function<void()> cleanup)
        : observer_(lv_subject_add_observer(subject, cb, user_data)),
          cleanup_(std::move(cleanup)) {}

    ~ObserverGuard() { reset(); }

    ObserverGuard(ObserverGuard&& other) noexcept
        : observer_(std::exchange(other.observer_, nullptr)),
          cleanup_(std::move(other.cleanup_)) {}

    ObserverGuard& operator=(ObserverGuard&& other) noexcept {
        if (this != &other) {
            reset();
            observer_ = std::exchange(other.observer_, nullptr);
            cleanup_ = std::move(other.cleanup_);
        }
        return *this;
    }

    ObserverGuard(const ObserverGuard&) = delete;
    ObserverGuard& operator=(const ObserverGuard&) = delete;

    static void invalidate_all() {
        s_subjects_valid.store(false, std::memory_order_release);
    }
    static void revalidate_all() {
        s_subjects_valid.store(true, std::memory_order_release);
    }

    void reset() {
        if (observer_) {
            if (s_subjects_valid.load(std::memory_order_acquire) &&
                lv_is_initialized()) {
                lv_observer_remove(observer_);
            }
            observer_ = nullptr;
            if (cleanup_) {
                cleanup_();
                cleanup_ = nullptr;
            }
        }
    }

    void release() {
        observer_ = nullptr;
        cleanup_ = nullptr;
    }

    explicit operator bool() const { return observer_ != nullptr; }
    lv_observer_t* get() const { return observer_; }

  private:
    static inline std::atomic<bool> s_subjects_valid{true};
    lv_observer_t* observer_ = nullptr;
    std::function<void()> cleanup_;
};
