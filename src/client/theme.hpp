#pragma once
#include <cstdint>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

namespace client::ui::theme {
inline constexpr std::uint32_t canvas = 0x161b22;
inline constexpr std::uint32_t sidebar = 0x0d1117;
inline constexpr std::uint32_t input = 0x21262d;
inline constexpr std::uint32_t selection = 0x263750;
inline constexpr std::uint32_t text = 0xe6edf3;
inline constexpr std::uint32_t secondary = 0x9da7b3;
inline constexpr std::uint32_t outline = 0x303943;
inline constexpr std::uint32_t accent = 0x79c0ff;
inline constexpr std::uint32_t success = 0x7ee787;
inline constexpr std::uint32_t warning = 0xe3b341;
inline constexpr std::uint32_t error = 0xff7b72;

inline ftxui::Color
rgb(std::uint32_t value) {
    return ftxui::Color::RGB(
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value)
    );
}

inline ftxui::Element
editor(ftxui::InputState state) {
    // Input's default transform inverts the entire focused field, not just its cursor.
    return state.element
           | ftxui::color(rgb(state.is_placeholder ? secondary : text))
           | ftxui::bgcolor(rgb(input));
}

inline ftxui::Element
room(const ftxui::EntryState& state) {
    auto value = ftxui::text((state.active ? "> " : "  ") + state.label)
                 | ftxui::color(rgb(state.active ? accent : secondary));
    if (state.active)
        value |= ftxui::bold | ftxui::bgcolor(rgb(selection));
    if (state.focused)
        value |= ftxui::underlined;
    return value;
}

inline ftxui::Element
action(const ftxui::EntryState& state, bool enabled) {
    auto value = ftxui::text("  " + state.label + " ↵  ")
                 | ftxui::bold
                 | ftxui::color(rgb(enabled ? accent : secondary))
                 | ftxui::bgcolor(rgb(input));
    return state.focused ? value | ftxui::underlined : value;
}
} // namespace client::ui::theme
