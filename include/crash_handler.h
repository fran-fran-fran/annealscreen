// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file crash_handler.h
 * @brief Async-signal-safe crash handler — writes crash.txt on SIGSEGV/SIGABRT/SIGBUS/SIGFPE
 *
 * Adapted from HelixScreen's crash_handler. Core diagnostics only:
 * signal, registers, backtrace, callback tag, version, uptime.
 *
 * Crash file format (line-oriented text):
 * @code
 * signal:11
 * name:SIGSEGV
 * version:0.1.0
 * timestamp:1707350400
 * uptime:3600
 * fault_addr:0x00000000
 * reg_pc:0x0040abcd
 * reg_sp:0x7ffd12345678
 * load_base:0x00400000
 * queue_callback:AnnealrState::apply_status
 * bt:0x0040abcd
 * bt:0x0040ef01
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>

namespace crash_handler {

/// Install signal handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE.
/// Path is copied to a static buffer (no heap in the signal handler).
void install(const std::string& crash_file_path, const char* version);

/// Uninstall signal handlers (restore SIG_DFL).
void uninstall();

/// Check if a crash file exists from a previous run.
bool has_crash_file(const std::string& crash_file_path);

/// Log the crash file contents via spdlog, then delete it.
void report_and_remove(const std::string& crash_file_path);

/// Register a pointer to the UpdateQueue's current callback tag.
/// The signal handler reads this volatile pointer to name the active callback.
void register_callback_tag_ptr(volatile const char* const* tag_ptr);

/// Register the previous-tag ring (recently completed callbacks).
void register_previous_tag_ring(volatile const char* const* ring,
                                unsigned int capacity,
                                volatile const unsigned int* next);

/// Write a crash record for an uncaught C++ exception (async-signal-safe).
void write_exception_record(const char* what) noexcept;

} // namespace crash_handler
