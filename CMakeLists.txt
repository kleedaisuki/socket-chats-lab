cmake_minimum_required(VERSION 3.25)
project(SocketChatsLab VERSION 0.1.0 LANGUAGES CXX)

option(CHAT_WITH_POSTGRES "Build the PostgreSQL server backend (libpq)" ON)
option(CHAT_FORCE_POLL "Use poll even on Linux (portability testing)" OFF)
option(CHAT_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
include(CTest)

add_library(chat_options INTERFACE)
if(MSVC)
  target_compile_options(chat_options INTERFACE /W4 /permissive- /utf-8)
  target_compile_definitions(chat_options INTERFACE _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(chat_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
if(CHAT_FORCE_POLL)
  target_compile_definitions(chat_options INTERFACE CHAT_FORCE_POLL=1)
endif()
if(CHAT_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "CHAT_SANITIZERS requires GCC or Clang")
  endif()
  target_compile_options(chat_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(chat_options INTERFACE -fsanitize=address,undefined)
endif()

add_subdirectory(src/core)
add_subdirectory(src/server)
add_subdirectory(src/client)

if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME ops COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_ops.py")
  add_test(NAME cli
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli.py"
      --server $<TARGET_FILE:chat-server>
      --daemon $<TARGET_FILE:chat-daemon>
      --client $<TARGET_FILE:chat>)
  set_tests_properties(cli PROPERTIES TIMEOUT 30)
  add_test(NAME journey_memory
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/journey.py"
      --server $<TARGET_FILE:chat-server>
      --daemon $<TARGET_FILE:chat-daemon>
      --client $<TARGET_FILE:chat>)
  set_tests_properties(journey_memory PROPERTIES TIMEOUT 120)
  add_test(NAME server_publication
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/server_publication.py"
      --server $<TARGET_FILE:chat-server>)
  set_tests_properties(server_publication PROPERTIES TIMEOUT 60)
  if(UNIX)
    add_test(NAME terminal_journey
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/terminal_journey.py"
        --server $<TARGET_FILE:chat-server>
        --daemon $<TARGET_FILE:chat-daemon>
        --client $<TARGET_FILE:chat>)
    set_tests_properties(terminal_journey PROPERTIES TIMEOUT 60)
  endif()
endif()

install(TARGETS chat-server chat-daemon chat RUNTIME DESTINATION bin)
