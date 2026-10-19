"""Apply narrow Windows input fixes to the pinned FTXUI 7.0.3 source.

Keep this patch explicit and fail closed on source drift. It can be removed when
an upstream release preserves split UTF-16 pairs, key repeats, and raw input modes.
"""

from pathlib import Path
import sys


PATCHES = (
    (
        "  std::string output_buffer;\n",
        "  std::string output_buffer;\n"
        "#if defined(_WIN32)\n"
        "  wchar_t pending_high_surrogate_ = 0;\n"
        "#endif\n",
    ),
    (
        "  in_mode |= enable_window_input;\n",
        "  in_mode |= enable_window_input;\n"
        "  // QuickEdit freezes console output during selection; Ctrl-C belongs to the app.\n"
        "  in_mode |= ENABLE_EXTENDED_FLAGS;\n"
        "  in_mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_PROCESSED_INPUT);\n",
    ),
    (
        "  std::wstring wstring;\n  for (const auto& r : records) {\n",
        """  // Retain a high surrogate across separate nonblocking console batches.
  const auto add_code_unit = [&](wchar_t wc) {
    const auto emit = [&](std::wstring_view units) {
      for (const char byte : to_string(units)) {
        terminal_input_parser.Add(byte);
      }
    };
    if (wc >= 0xd800 && wc <= 0xdbff) {
      if (pending_high_surrogate_) {
        emit(L"\ufffd");
      }
      pending_high_surrogate_ = wc;
      return;
    }
    if (pending_high_surrogate_) {
      const wchar_t units[] = {pending_high_surrogate_, wc};
      pending_high_surrogate_ = 0;
      if (wc >= 0xdc00 && wc <= 0xdfff) {
        emit(std::wstring_view(units, 2));
        return;
      }
      emit(L"\ufffd");
    }
    if (wc >= 0xdc00 && wc <= 0xdfff) {
      emit(L"\ufffd");
      return;
    }
    emit(std::wstring_view(&wc, 1));
  };
  for (const auto& r : records) {
""",
    ),
    (
        """        const wchar_t wc = key_event.uChar.UnicodeChar;
        wstring += wc;
        if (wc >= 0xd800 && wc <= 0xdbff) {
          // Wait for the Low Surrogate to arrive in the next record.
          continue;
        }
        for (auto it : to_string(wstring)) {
          terminal_input_parser.Add(it);
        }
        wstring.clear();
""",
        """        const wchar_t wc = key_event.uChar.UnicodeChar;
        if (wc == 0) {
          continue;
        }
        for (unsigned repeat = 0; repeat < key_event.wRepeatCount; ++repeat) {
          add_code_unit(wc);
        }
""",
    ),
)


def patch(source: Path) -> bool:
    path = source / "src/ftxui/component/app.cpp"
    text = path.read_text(encoding="utf-8")
    updated = text
    for before, after in PATCHES:
        if updated.count(after) == 1:
            continue
        if updated.count(before) != 1:
            raise RuntimeError(f"Pinned FTXUI source context changed: {before[:70]!r}")
        updated = updated.replace(before, after, 1)
    if text == updated:
        return False
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(updated)
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: patch_ftxui.py SOURCE_DIRECTORY")
    print("FTXUI Windows input patch:", "applied" if patch(Path(sys.argv[1])) else "already applied")
