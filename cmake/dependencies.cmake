include(FetchContent)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# Scope third-party options so the application's BUILD_SHARED_LIBS policy is unchanged.
function(chat_fetch_ftxui)
  set(BUILD_SHARED_LIBS OFF)
  set(FTXUI_BUILD_DOCS OFF)
  set(FTXUI_BUILD_EXAMPLES OFF)
  set(FTXUI_BUILD_MODULES OFF)
  set(FTXUI_BUILD_TESTS OFF)
  set(FTXUI_BUILD_TESTS_FUZZER OFF)
  set(FTXUI_ENABLE_INSTALL OFF)
  set(FTXUI_ENABLE_CCACHE OFF)
  set(FTXUI_CLANG_TIDY OFF)
  set(FTXUI_DEV_WARNINGS OFF)
  set(FTXUI_ENABLE_COVERAGE OFF)
  set(FTXUI_QUIET ON)
  FetchContent_Declare(ftxui
    URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/refs/tags/v7.0.3
    URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/patch_ftxui.py" <SOURCE_DIR>
  )
  FetchContent_MakeAvailable(ftxui)
  install(FILES "${ftxui_SOURCE_DIR}/LICENSE"
    DESTINATION share/licenses/socket-chats-lab RENAME FTXUI-LICENSE)
endfunction()

chat_fetch_ftxui()
