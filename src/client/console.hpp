#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdexcept>
#include <windows.h>
#endif

namespace client
{
// Scope outside every FTXUI object: color detection can change modes before Loop starts.
class ConsoleState
{
  public:
    ConsoleState()
    {
#ifdef _WIN32
        if (!GetConsoleMode(input_, &input_mode_) || !GetConsoleMode(output_, &output_mode_))
            throw std::runtime_error("Cannot capture console modes");
        input_cp_ = GetConsoleCP();
        output_cp_ = GetConsoleOutputCP();
        if (!input_cp_ || !output_cp_)
            throw std::runtime_error("Cannot capture console code pages");
        if (!SetConsoleCP(CP_UTF8) || !SetConsoleOutputCP(CP_UTF8)) {
            restore();
            throw std::runtime_error("Cannot enable UTF-8 console I/O");
        }
#endif
    }
    ~ConsoleState()
    {
#ifdef _WIN32
        restore();
#endif
    }
    ConsoleState(const ConsoleState&) = delete;
    ConsoleState& operator=(const ConsoleState&) = delete;

  private:
#ifdef _WIN32
    HANDLE input_ = GetStdHandle(STD_INPUT_HANDLE), output_ = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD input_mode_ = 0, output_mode_ = 0;
    UINT input_cp_ = 0, output_cp_ = 0;

    void restore() noexcept
    {
        SetConsoleMode(input_, input_mode_);
        SetConsoleMode(output_, output_mode_);
        SetConsoleCP(input_cp_);
        SetConsoleOutputCP(output_cp_);
    }
#endif
};
} // namespace client
