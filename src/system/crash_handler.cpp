// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from HelixScreen's crash_handler.cpp (GPL-3.0-or-later, 356C LLC).
// Stripped: telemetry integration, breadcrumb ring, heap snapshot, LVGL event
// hook, mock/test crash, crash reporter UI, /proc/self/maps dump.
// Kept: signal handler, register dump, backtrace (glibc + FP walk), callback
// tag ring, crash.txt writer, exception record.

#include "crash_handler.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <string>
#include <unistd.h>

#ifdef __linux__
#include <ucontext.h>
#endif

#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#include <link.h>
#define HAVE_DL_ITERATE_PHDR 1
#endif

// ═══════════════════════════════════════════════════════════════════════════
// Static buffers — NO heap in the signal handler
// ═══════════════════════════════════════════════════════════════════════════

static constexpr size_t MAX_PATH_LEN = 512;
static char s_crash_path[MAX_PATH_LEN] = {};
static char s_version[64] = {};
static volatile sig_atomic_t s_installed = 0;
static volatile sig_atomic_t s_crash_file_written = 0;
static time_t s_start_time = 0;
static uintptr_t s_load_base = 0;
static uintptr_t s_text_start = 0;
static uintptr_t s_text_end = 0;

// Callback tag pointers (registered by UpdateQueue)
static volatile const char* const* s_callback_tag_ptr = nullptr;
static volatile const char* const* s_previous_tag_ring = nullptr;
static unsigned int s_previous_tag_capacity = 0;
static volatile const unsigned int* s_previous_tag_next = nullptr;

// Saved previous signal actions
static struct sigaction s_old_sigsegv = {};
static struct sigaction s_old_sigabrt = {};
static struct sigaction s_old_sigbus = {};
static struct sigaction s_old_sigfpe = {};

// ═══════════════════════════════════════════════════════════════════════════
// ELF load base discovery
// ═══════════════════════════════════════════════════════════════════════════

#if defined(__linux__)
extern "C" {
extern char __executable_start[];
extern char _etext[];
}
#endif

#ifdef HAVE_DL_ITERATE_PHDR
static int find_load_base_cb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
        s_load_base = static_cast<uintptr_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
// Async-signal-safe helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

static void safe_write(int fd, const char* str) {
    if (!str) return;
    size_t len = 0;
    while (str[len] != '\0') ++len;
    (void)write(fd, str, len);
}

static char* int_to_str(char* buf, size_t buf_size, long value) {
    if (buf_size == 0) return buf;
    bool negative = (value < 0);
    unsigned long uval = negative
        ? static_cast<unsigned long>(-value)
        : static_cast<unsigned long>(value);
    char* end = buf + buf_size - 1;
    *end = '\0';
    char* p = end;
    if (uval == 0) { --p; *p = '0'; }
    else { while (uval > 0 && p > buf) { --p; *p = '0' + (uval % 10); uval /= 10; } }
    if (negative && p > buf) { --p; *p = '-'; }
    return p;
}

static char* ptr_to_hex(char* buf, size_t buf_size, uintptr_t value) {
    if (buf_size < 3) return buf;
    static const char hex_chars[] = "0123456789abcdef";
    char* end = buf + buf_size - 1;
    *end = '\0';
    char* p = end;
    if (value == 0) { --p; *p = '0'; }
    else { while (value > 0 && p > buf + 2) { --p; *p = hex_chars[value & 0xF]; value >>= 4; } }
    --p; *p = 'x';
    --p; *p = '0';
    return p;
}

static const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    default:      return "UNKNOWN";
    }
}

static const char* fault_code_name(int sig, int code) {
    if (sig == SIGSEGV) {
        switch (code) {
        case SEGV_MAPERR: return "SEGV_MAPERR";
        case SEGV_ACCERR: return "SEGV_ACCERR";
        default: return "UNKNOWN";
        }
    } else if (sig == SIGBUS) {
        switch (code) {
        case BUS_ADRALN: return "BUS_ADRALN";
        case BUS_ADRERR: return "BUS_ADRERR";
        default: return "UNKNOWN";
        }
    } else if (sig == SIGFPE) {
        switch (code) {
        case FPE_INTDIV: return "FPE_INTDIV";
        case FPE_FLTDIV: return "FPE_FLTDIV";
        default: return "UNKNOWN";
        }
    }
    return "UNKNOWN";
}

// ═══════════════════════════════════════════════════════════════════════════
// Frame-pointer chain walker (from HelixScreen)
// ═══════════════════════════════════════════════════════════════════════════

static int fp_walk_backtrace(int fd, uintptr_t initial_fp, uintptr_t sp,
                              uintptr_t word_size) {
#if defined(__aarch64__) || defined(__x86_64__) || defined(__arm__)
    constexpr uintptr_t kMaxStackSize = 16 * 1024 * 1024;
    constexpr int kMaxFrames = 48;
    if (initial_fp == 0 || sp == 0 || word_size == 0) return 0;

    char hex_buf[32];
    int frames = 0;
    uintptr_t stack_hi = sp + kMaxStackSize;
    if (stack_hi < sp) stack_hi = ~static_cast<uintptr_t>(0);
    uintptr_t fp = initial_fp;
    uintptr_t prev_fp = 0;

    for (int i = 0; i < kMaxFrames; ++i) {
        if ((fp & (word_size - 1)) != 0) break;
        if (fp < sp || fp + 2 * word_size > stack_hi) break;
        if (fp <= prev_fp && prev_fp != 0) break;

        uintptr_t saved_fp, saved_lr;
        if (word_size == 8) {
            saved_fp = *reinterpret_cast<const volatile uint64_t*>(fp);
            saved_lr = *reinterpret_cast<const volatile uint64_t*>(fp + word_size);
        } else {
            saved_fp = *reinterpret_cast<const volatile uint32_t*>(fp);
            saved_lr = *reinterpret_cast<const volatile uint32_t*>(fp + word_size);
        }

#if defined(__arm__)
        uintptr_t lr_addr = saved_lr & ~static_cast<uintptr_t>(1);
        bool lr_in_text = (s_text_start != 0 && s_text_end > s_text_start
                           && lr_addr >= s_text_start && lr_addr < s_text_end);
        if (lr_in_text && lr_addr != 0) {
            safe_write(fd, "bt:");
            safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), lr_addr));
            safe_write(fd, "\n");
            ++frames;
        }
#else
        if (saved_lr != 0) {
            safe_write(fd, "bt:");
            safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), saved_lr));
            safe_write(fd, "\n");
            ++frames;
        }
#endif
        if (saved_fp == 0) break;
        prev_fp = fp;
        fp = saved_fp;
    }
    return frames;
#else
    (void)fd; (void)initial_fp; (void)sp; (void)word_size;
    return 0;
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
// The signal handler — async-signal-safe ONLY
// ═══════════════════════════════════════════════════════════════════════════

static void crash_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    sig_atomic_t expected = 0;
    if (!__atomic_compare_exchange_n(&s_crash_file_written, &expected, 1, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        struct sigaction sa_dfl = {};
        sa_dfl.sa_handler = SIG_DFL;
        sigaction(sig, &sa_dfl, nullptr);
        raise(sig);
        _exit(128 + sig);
    }

    int fd = open(s_crash_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        safe_write(STDERR_FILENO, "\n=== ANNEAL_CRASH_DUMP ===\n");
        fd = STDERR_FILENO;
    }

    char num_buf[32];
    char hex_buf[32];

    // Signal info
    safe_write(fd, "signal:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), sig));
    safe_write(fd, "\nname:");
    safe_write(fd, signal_name(sig));
    safe_write(fd, "\nversion:");
    safe_write(fd, s_version);
    safe_write(fd, "\n");

    // Timestamp + uptime
    time_t now = time(nullptr);
    safe_write(fd, "timestamp:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), static_cast<long>(now)));
    safe_write(fd, "\n");
    if (s_start_time > 0 && now >= s_start_time) {
        safe_write(fd, "uptime:");
        safe_write(fd, int_to_str(num_buf, sizeof(num_buf),
                                   static_cast<long>(now - s_start_time)));
        safe_write(fd, "\n");
    }

    // Fault address
    if (info) {
        safe_write(fd, "fault_addr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                   reinterpret_cast<uintptr_t>(info->si_addr)));
        safe_write(fd, "\nfault_code:");
        safe_write(fd, int_to_str(num_buf, sizeof(num_buf), info->si_code));
        safe_write(fd, "\nfault_code_name:");
        safe_write(fd, fault_code_name(sig, info->si_code));
        safe_write(fd, "\n");
    }

    // Register state from ucontext
#if defined(__linux__)
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
#if defined(__aarch64__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.pc));
        safe_write(fd, "\nreg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.sp));
        safe_write(fd, "\nreg_lr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.regs[30]));
        safe_write(fd, "\n");
#elif defined(__arm__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_pc));
        safe_write(fd, "\nreg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_sp));
        safe_write(fd, "\nreg_lr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_lr));
        safe_write(fd, "\nreg_fp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_fp));
        safe_write(fd, "\n");
#elif defined(__x86_64__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RIP]));
        safe_write(fd, "\nreg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RSP]));
        safe_write(fd, "\nreg_bp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RBP]));
        safe_write(fd, "\n");
#endif
    }
#endif

    // Load base
    if (s_load_base != 0) {
        safe_write(fd, "load_base:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), s_load_base));
        safe_write(fd, "\n");
    }

    // Callback tag
    if (s_callback_tag_ptr) {
        const char* tag = const_cast<const char*>(*s_callback_tag_ptr);
        if (tag) {
            safe_write(fd, "queue_callback:");
            safe_write(fd, tag);
            safe_write(fd, "\n");
        }
    }

    // Previous callback tag ring
    if (s_previous_tag_ring && s_previous_tag_capacity > 0 && s_previous_tag_next) {
        unsigned int next = *s_previous_tag_next;
        static const char* const kLabels[] = {
            "queue_prev:", "queue_prev2:", "queue_prev3:", "queue_prev4:",
        };
        unsigned int limit = s_previous_tag_capacity < 4 ? s_previous_tag_capacity : 4;
        for (unsigned int i = 0; i < limit; ++i) {
            unsigned int idx = (next + s_previous_tag_capacity - 1 - i) % s_previous_tag_capacity;
            const char* prev = const_cast<const char*>(s_previous_tag_ring[idx]);
            if (!prev) break;
            safe_write(fd, kLabels[i]);
            safe_write(fd, prev);
            safe_write(fd, "\n");
        }
    }

    // ── Backtrace ────────────────────────────────────────────────────

    // Inject ucontext PC+LR first
#if defined(__linux__)
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
#if defined(__aarch64__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.pc));
        safe_write(fd, "\nbt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.regs[30]));
        safe_write(fd, "\n");
#elif defined(__arm__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_pc));
        safe_write(fd, "\nbt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_lr));
        safe_write(fd, "\n");
#elif defined(__x86_64__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RIP]));
        safe_write(fd, "\n");
#endif
    }
#endif

    // glibc backtrace()
    int bt_source_written = 0;
#ifdef HAVE_BACKTRACE
    {
        void* bt_buf[64];
        int bt_count = backtrace(bt_buf, 64);
        if (bt_count > 2) {
            safe_write(fd, "bt_source:backtrace\n");
            bt_source_written = 1;
            for (int i = 2; i < bt_count; ++i) {
                safe_write(fd, "bt:");
                safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                           reinterpret_cast<uintptr_t>(bt_buf[i])));
                safe_write(fd, "\n");
            }
        }
    }
#endif

    // FP walk fallback
#if defined(__linux__)
    if (!bt_source_written && ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
        uintptr_t fp_val = 0, sp_val = 0;
        uintptr_t word_size = sizeof(void*);
#if defined(__aarch64__)
        // x29 = FP on AArch64 (regs[29])
        fp_val = uctx->uc_mcontext.regs[29];
        sp_val = uctx->uc_mcontext.sp;
#elif defined(__arm__)
        fp_val = uctx->uc_mcontext.arm_r7; // Thumb mode FP
        sp_val = uctx->uc_mcontext.arm_sp;
#elif defined(__x86_64__)
        fp_val = uctx->uc_mcontext.gregs[REG_RBP];
        sp_val = uctx->uc_mcontext.gregs[REG_RSP];
#endif
        if (fp_val != 0) {
            safe_write(fd, "bt_source:fp_walk\n");
            fp_walk_backtrace(fd, fp_val, sp_val, word_size);
        }
    }
#endif

    if (fd != STDERR_FILENO) close(fd);

    // Re-raise with default handler
    struct sigaction sa_dfl = {};
    sa_dfl.sa_handler = SIG_DFL;
    sigaction(sig, &sa_dfl, nullptr);
    raise(sig);
    _exit(128 + sig);
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

void crash_handler::install(const std::string& crash_file_path,
                             const char* version) {
    if (s_installed) return;

    // Copy path and version to static buffers
    size_t len = crash_file_path.size();
    if (len >= MAX_PATH_LEN) len = MAX_PATH_LEN - 1;
    std::memcpy(s_crash_path, crash_file_path.c_str(), len);
    s_crash_path[len] = '\0';

    if (version) {
        size_t vlen = std::strlen(version);
        if (vlen >= sizeof(s_version)) vlen = sizeof(s_version) - 1;
        std::memcpy(s_version, version, vlen);
        s_version[vlen] = '\0';
    }

    s_start_time = time(nullptr);
    s_crash_file_written = 0;

    // Discover load base (ASLR offset)
#ifdef HAVE_DL_ITERATE_PHDR
    dl_iterate_phdr(find_load_base_cb, nullptr);
#endif
#if defined(__linux__)
    s_text_start = reinterpret_cast<uintptr_t>(__executable_start);
    s_text_end = reinterpret_cast<uintptr_t>(_etext);
#endif

    // Install signal handlers
    struct sigaction sa = {};
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &s_old_sigsegv);
    sigaction(SIGABRT, &sa, &s_old_sigabrt);
    sigaction(SIGBUS,  &sa, &s_old_sigbus);
    sigaction(SIGFPE,  &sa, &s_old_sigfpe);

    s_installed = 1;
    spdlog::debug("[CrashHandler] Installed, crash file: {}", crash_file_path);
}

void crash_handler::uninstall() {
    if (!s_installed) return;
    sigaction(SIGSEGV, &s_old_sigsegv, nullptr);
    sigaction(SIGABRT, &s_old_sigabrt, nullptr);
    sigaction(SIGBUS,  &s_old_sigbus,  nullptr);
    sigaction(SIGFPE,  &s_old_sigfpe,  nullptr);
    s_installed = 0;
}

bool crash_handler::has_crash_file(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

void crash_handler::report_and_remove(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    f.close();

    spdlog::error("╔══════════════════════════════════════════════════╗");
    spdlog::error("║  PREVIOUS CRASH DETECTED                        ║");
    spdlog::error("╚══════════════════════════════════════════════════╝");
    for (const auto& line : contents) {
        // Log line by line
    }
    // Log the whole file
    spdlog::error("Crash report:\n{}", contents);

    std::remove(path.c_str());
    spdlog::info("[CrashHandler] Crash file removed: {}", path);
}

void crash_handler::register_callback_tag_ptr(volatile const char* const* tag_ptr) {
    s_callback_tag_ptr = tag_ptr;
}

void crash_handler::register_previous_tag_ring(volatile const char* const* ring,
                                                unsigned int capacity,
                                                volatile const unsigned int* next) {
    s_previous_tag_ring = ring;
    s_previous_tag_capacity = capacity;
    s_previous_tag_next = next;
}

void crash_handler::write_exception_record(const char* what) noexcept {
    if (!s_installed) return;
    sig_atomic_t expected = 0;
    if (!__atomic_compare_exchange_n(&s_crash_file_written, &expected, 1, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }

    int fd = open(s_crash_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return;

    char num_buf[32];
    safe_write(fd, "signal:6\nname:EXCEPTION\nversion:");
    safe_write(fd, s_version);
    safe_write(fd, "\n");

    time_t now = time(nullptr);
    safe_write(fd, "timestamp:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), static_cast<long>(now)));
    safe_write(fd, "\n");

    if (what) {
        safe_write(fd, "exception:");
        safe_write(fd, what);
        safe_write(fd, "\n");
    }

    close(fd);
}
