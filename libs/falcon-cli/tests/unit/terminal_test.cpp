/**
 * @file terminal_test.cpp
 * @brief Unit tests for the header-only terminal helpers in terminal.hpp
 */

#include "terminal.hpp"

#include <gtest/gtest.h>

#include <string>

namespace term = falcon::cli::term;

namespace {

// color_enabled() is a process-wide static reference; save/restore it around
// each test so the tests stay independent from execution order.
class TerminalColorGuard {
public:
    TerminalColorGuard() : previous_(term::color_enabled()) {}
    ~TerminalColorGuard() { term::color_enabled() = previous_; }

private:
    bool previous_;
};

} // namespace

TEST(TerminalHelpers, AnsiSequencesWhenColorEnabled) {
    TerminalColorGuard guard;
    term::color_enabled() = true;

    EXPECT_EQ("\033[31m", term::fg(term::Color::Red));
    EXPECT_EQ("\033[36m", term::fg(term::Color::Cyan));
    EXPECT_EQ("\033[1m", term::fg(term::Color::Bold));
    EXPECT_EQ("\033[41m", term::bg(term::BgColor::Red));
    EXPECT_EQ("\033[44m", term::bg(term::BgColor::Blue));
    EXPECT_EQ("\033[0m", term::reset());
    EXPECT_EQ("\033[1m", term::bold());

    EXPECT_EQ("\033[31mtext\033[0m", term::red("text"));
    EXPECT_EQ("\033[32mgood\033[0m", term::green("good"));
    EXPECT_EQ("\033[33mwarn\033[0m", term::yellow("warn"));
    EXPECT_EQ("\033[36minfo\033[0m", term::cyan("info"));
    EXPECT_EQ("\033[34mlink\033[0m", term::blue("link"));

    EXPECT_EQ("\033[32m[OK]\033[0m", term::ok());
    EXPECT_EQ("\033[31m[ERR]\033[0m", term::err());
    EXPECT_EQ("\033[33m[WARN]\033[0m", term::warn());
    EXPECT_EQ("\033[36m[INFO]\033[0m", term::info());
}

TEST(TerminalHelpers, PlainOutputWhenColorDisabled) {
    TerminalColorGuard guard;
    term::color_enabled() = false;

    EXPECT_TRUE(term::fg(term::Color::Red).empty());
    EXPECT_TRUE(term::fg(term::Color::Cyan).empty());
    EXPECT_TRUE(term::bg(term::BgColor::Green).empty());
    EXPECT_TRUE(term::reset().empty());
    EXPECT_TRUE(term::bold().empty());

    EXPECT_EQ("plain", term::red("plain"));
    EXPECT_EQ("good", term::green("good"));
    EXPECT_EQ("warn", term::yellow("warn"));
    EXPECT_EQ("info", term::cyan("info"));
    EXPECT_EQ("link", term::blue("link"));

    EXPECT_EQ("[OK]", term::ok());
    EXPECT_EQ("[ERR]", term::err());
    EXPECT_EQ("[WARN]", term::warn());
    EXPECT_EQ("[INFO]", term::info());
}

TEST(TerminalHelpers, CursorControlSequencesAreUnconditional) {
    TerminalColorGuard guard;
    term::color_enabled() = false;

    // Cursor helpers ignore the color switch.
    EXPECT_EQ("\033[2A", term::move_up(2));
    EXPECT_EQ("\033[7B", term::move_down(7));
    EXPECT_EQ("\033[2K\r", term::clear_line());

    term::color_enabled() = true;
    EXPECT_EQ("\033[2A", term::move_up(2));
    EXPECT_EQ("\033[7B", term::move_down(7));
    EXPECT_EQ("\033[2K\r", term::clear_line());
}

TEST(TerminalHelpers, EnableAnsiAndIsTerminalAreCallable) {
    EXPECT_NO_FATAL_FAILURE(term::enable_ansi());
    EXPECT_NO_FATAL_FAILURE(term::enable_ansi());  // second call is idempotent

    const bool first = term::is_terminal();
    EXPECT_EQ(first, term::is_terminal());  // stable within the same process
}
