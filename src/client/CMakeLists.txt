add_executable(chat-daemon daemon.cpp)
add_executable(chat tui.cpp)
target_link_libraries(chat-daemon PRIVATE chat_core Threads::Threads)
target_link_libraries(chat PRIVATE chat_core Threads::Threads)
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
endif()
