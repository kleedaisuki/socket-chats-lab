#include "../src/client/console.hpp"
#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace {
void
check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void
inject(HANDLE input, wchar_t unit, WORD repeats = 1) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = repeats;
    record.Event.KeyEvent.uChar.UnicodeChar = unit;
    DWORD written = 0;
    check(
        WriteConsoleInputW(input, &record, 1, &written) && written == 1,
        "WriteConsoleInputW failed"
    );
}

void
run() {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD before_input = 0, before_output = 0;
    check(
        GetConsoleMode(input, &before_input) && GetConsoleMode(output, &before_output),
        "Fixture requires a hidden pseudo-console, not redirected stdin"
    );
    const auto before_input_cp = GetConsoleCP(), before_output_cp = GetConsoleOutputCP();
    std::string content;
    {
        client::ConsoleState console;
        auto screen = ftxui::ScreenInteractive::Fullscreen();
        screen.TrackMouse(false);
        ftxui::InputOption options;
        options.multiline = false;
        auto editor = ftxui::Input(&content, options);
        ftxui::Loop loop(&screen, editor);
        loop.RunOnce();
        DWORD mode = 0;
        check(GetConsoleMode(input, &mode), "Cannot inspect interactive input mode");
        check(
            !(mode
              & (ENABLE_QUICK_EDIT_MODE
                 | ENABLE_LINE_INPUT
                 | ENABLE_ECHO_INPUT
                 | ENABLE_PROCESSED_INPUT)),
            "Interactive mode must disable cooked input, QuickEdit and OS Ctrl-C"
        );

        inject(input, static_cast<wchar_t>(0xd83c));
        loop.RunOnce();
        check(content.empty(), "High surrogate must remain pending without text or blocking");
        inject(input, static_cast<wchar_t>(0xdf38));
        loop.RunOnce();
        check(content == "\xf0\x9f\x8c\xb8", "Split-batch emoji was corrupted or lost");

        // The console host rewrites injected VT records with repeat=1. Exercise the
        // native repeat-record path without that transformation, then restore VT.
        check(
            SetConsoleMode(input, mode & ~ENABLE_VIRTUAL_TERMINAL_INPUT),
            "Cannot enter native repeat-record mode"
        );
        inject(input, L'x', 7);
        INPUT_RECORD peek{};
        DWORD peeked = 0;
        check(
            PeekConsoleInputW(input, &peek, 1, &peeked)
                && peeked == 1
                && peek.Event.KeyEvent.wRepeatCount == 7,
            "Console host did not preserve the injected native repeat record"
        );
        loop.RunOnce();
        check(SetConsoleMode(input, mode), "Cannot restore VT input mode");
        if (content != "\xf0\x9f\x8c\xb8xxxxxxx")
            throw std::runtime_error(
                "Native repeat count was lost; content="
                + content
                + " size="
                + std::to_string(content.size())
            );
        inject(input, L'\x4f60');
        inject(input, L'\x597d');
        loop.RunOnce();
        check(
            content == "\xf0\x9f\x8c\xb8xxxxxxx\xe4\xbd\xa0\xe5\xa5\xbd",
            "Batched Chinese input was corrupted"
        );
        screen.Exit();
        loop.RunOnce();
    }
    DWORD after_input = 0, after_output = 0;
    check(
        GetConsoleMode(input, &after_input) && GetConsoleMode(output, &after_output),
        "Cannot inspect restored console mode"
    );
    if (before_input != after_input || before_output != after_output)
        throw std::runtime_error(
            "FTXUI failed to restore native console modes: input "
            + std::to_string(before_input)
            + " -> "
            + std::to_string(after_input)
            + "; output "
            + std::to_string(before_output)
            + " -> "
            + std::to_string(after_output)
        );
    check(
        before_input_cp == GetConsoleCP() && before_output_cp == GetConsoleOutputCP(),
        "Console guard failed to restore code pages"
    );
}
} // namespace

// The ConPTY test launcher supplies a result path; never allocate a visible console here.
int
main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    try {
        run();
        std::ofstream(
            argv[1]
        ) << "PASS split-surrogate repeat-count Chinese-batch mode-restoration\n";
        return 0;
    } catch (const std::exception& error) {
        std::ofstream(argv[1]) << "FAIL " << error.what() << '\n';
        return 1;
    }
}
