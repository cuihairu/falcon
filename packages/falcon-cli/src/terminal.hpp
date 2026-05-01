/**
 * @file terminal.hpp
 * @brief 跨平台终端颜色和光标控制
 */

#pragma once

#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace falcon::cli::term {

// ANSI color codes
enum class Color {
    Reset   = 0,
    Black   = 30,
    Red     = 31,
    Green   = 32,
    Yellow  = 33,
    Blue    = 34,
    Magenta = 35,
    Cyan    = 36,
    White   = 37,
    Bold    = 1,
};

enum class BgColor {
    Reset   = 0,
    Black   = 40,
    Red     = 41,
    Green   = 42,
    Yellow  = 43,
    Blue    = 44,
};

inline bool& color_enabled() {
    static bool enabled = true;
    return enabled;
}

inline bool is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

inline void enable_ansi() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode)) {
                SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
    }
#endif
}

inline std::string fg(Color c) {
    if (!color_enabled()) return "";
    return "\033[" + std::to_string(static_cast<int>(c)) + "m";
}

inline std::string bg(BgColor c) {
    if (!color_enabled()) return "";
    return "\033[" + std::to_string(static_cast<int>(c)) + "m";
}

inline std::string reset() {
    if (!color_enabled()) return "";
    return "\033[0m";
}

inline std::string bold() {
    if (!color_enabled()) return "";
    return "\033[1m";
}

// Convenience helpers
inline std::string red(const std::string& s)    { return fg(Color::Red) + s + reset(); }
inline std::string green(const std::string& s)   { return fg(Color::Green) + s + reset(); }
inline std::string yellow(const std::string& s)  { return fg(Color::Yellow) + s + reset(); }
inline std::string cyan(const std::string& s)    { return fg(Color::Cyan) + s + reset(); }
inline std::string blue(const std::string& s)    { return fg(Color::Blue) + s + reset(); }

// Status prefixes
inline std::string ok()   { return color_enabled() ? fg(Color::Green) + "[OK]" + reset() : "[OK]"; }
inline std::string err()  { return color_enabled() ? fg(Color::Red) + "[ERR]" + reset() : "[ERR]"; }
inline std::string warn() { return color_enabled() ? fg(Color::Yellow) + "[WARN]" + reset() : "[WARN]"; }
inline std::string info() { return color_enabled() ? fg(Color::Cyan) + "[INFO]" + reset() : "[INFO]"; }

// Cursor movement (for multi-line progress)
inline std::string move_up(int n)    { return "\033[" + std::to_string(n) + "A"; }
inline std::string move_down(int n)  { return "\033[" + std::to_string(n) + "B"; }
inline std::string clear_line()      { return "\033[2K\r"; }

} // namespace falcon::cli::term
