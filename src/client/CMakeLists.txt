add_executable(chat-daemon daemon.cpp)
add_library(chat_client_io session.cpp)
target_link_libraries(chat_client_io PUBLIC chat_core Threads::Threads)
add_library(chat_view view.cpp)
target_link_libraries(chat_view PUBLIC chat_core ftxui::dom ftxui::screen)
add_executable(chat tui.cpp plain.cpp)
target_link_libraries(chat-daemon PRIVATE chat_core Threads::Threads)
target_link_libraries(chat PRIVATE chat_client_io chat_view ftxui::component)
if(WIN32)
  target_link_libraries(chat-daemon PRIVATE advapi32)
endif()
if(BUILD_TESTING)
  add_executable(chat-client-tests ../../tests/client_tests.cpp)
  target_link_libraries(chat-client-tests PRIVATE chat_core)
  add_test(NAME client_cache COMMAND chat-client-tests)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME client_reconnect
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/client_journey.py"
      --server $<TARGET_FILE:chat-server>
      --daemon $<TARGET_FILE:chat-daemon>
      --client $<TARGET_FILE:chat>)
  set_tests_properties(client_reconnect PROPERTIES TIMEOUT 90)
  add_executable(chat-session-tests ../../tests/session_tests.cpp)
  target_link_libraries(chat-session-tests PRIVATE chat_client_io)
  add_test(NAME client_session COMMAND chat-session-tests)
  set_tests_properties(client_session PROPERTIES TIMEOUT 30)
  add_executable(chat-view-tests ../../tests/view_tests.cpp)
  target_link_libraries(chat-view-tests PRIVATE chat_view)
  add_test(NAME conversation_view COMMAND chat-view-tests)
  add_test(NAME dependency_patch
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/dependency_patch.py")
  if(UNIX)
    add_test(NAME tui_state_journey
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tui_state_journey.py"
        --server $<TARGET_FILE:chat-server>
        --daemon $<TARGET_FILE:chat-daemon>
        --client $<TARGET_FILE:chat>)
    set_tests_properties(tui_state_journey PROPERTIES TIMEOUT 90)
  endif()
  if(WIN32)
    add_executable(chat-ftxui-console-tests ../../tests/ftxui_console_input.cpp)
    target_link_libraries(chat-ftxui-console-tests PRIVATE chat_core ftxui::component)
    add_test(NAME windows_console_input
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/terminal_probe.py"
        --fixture $<TARGET_FILE:chat-ftxui-console-tests>
        --output "${PROJECT_SOURCE_DIR}/.cache/ftxui-native-input")
    set_tests_properties(windows_console_input PROPERTIES TIMEOUT 30)
    add_test(NAME windows_tui_journey
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/terminal_probe.py"
        --server $<TARGET_FILE:chat-server>
        --daemon $<TARGET_FILE:chat-daemon>
        --client $<TARGET_FILE:chat>
        --rounds 1 --editing --max-burst-ms 1000
        --output "${PROJECT_SOURCE_DIR}/.cache/windows-tui-journey")
    set_tests_properties(windows_tui_journey PROPERTIES TIMEOUT 120)
  endif()
endif()
