cmake_minimum_required(VERSION 3.20)
project(windbg_copilot VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static runtime for Release builds (no MSVC DLL dependencies)
# Debug and other configs use default dynamic runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Release>,MultiThreaded,$<IF:$<CONFIG:Debug>,MultiThreadedDebugDLL,MultiThreadedDLL>>")

# Windows-specific settings
if(NOT WIN32)
    message(STATUS "windbg_copilot only builds on Windows - skipping")
    return()
endif()

# Add libagents if building standalone (not part of monorepo)
if(NOT TARGET libagents)
    add_subdirectory(external/libagents)
endif()

# windbg_copilot DLL
add_library(windbg_copilot SHARED
    main.cpp
    output_capture.cpp
    settings.cpp
    session_store.cpp
    dml_output.cpp
    windbg_client.cpp
)

# Link with libagents and Windows debugging libraries
target_link_libraries(windbg_copilot
    PRIVATE
        libagents         # Unified provider library
        dbgeng            # Debugging Engine API
        dbghelp           # Debug Help Library
)

# Export symbols for WinDbg extension callbacks
target_compile_definitions(windbg_copilot PRIVATE
    _WINDOWS
    UNICODE
    _UNICODE
)

# Set output name without lib prefix
set_target_properties(windbg_copilot PROPERTIES
    PREFIX ""
    OUTPUT_NAME "windbg_copilot"
)

# Standalone tests may live under this repo or the top-level tests/windbg directory.
set(WINDBG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
if(NOT EXISTS "${WINDBG_TESTS_DIR}/session_restore_test.cpp")
    set(WINDBG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tests/windbg")
endif()

# Session restore test (standalone SDK test)
if(EXISTS "${WINDBG_TESTS_DIR}/session_restore_test.cpp")
    add_executable(session_restore_test
        ${WINDBG_TESTS_DIR}/session_restore_test.cpp
    )
    target_link_libraries(session_restore_test PRIVATE libagents)
endif()

# MCP tool test (verify MCP tools work)
if(EXISTS "${WINDBG_TESTS_DIR}/test_mcp_tool.cpp")
    add_executable(test_mcp_tool
        ${WINDBG_TESTS_DIR}/test_mcp_tool.cpp
    )
    target_link_libraries(test_mcp_tool PRIVATE libagents)
endif()

# Repro test for MCP tool visibility issue
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/repro/CMakeLists.txt")
    add_subdirectory(repro)
endif()

