cmake_minimum_required(VERSION 3.20)
project(windbg_agent VERSION 1.0.2 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static runtime for Release builds (no MSVC DLL dependencies)
# Debug and other configs use default dynamic runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Release>,MultiThreaded,$<IF:$<CONFIG:Debug>,MultiThreadedDebugDLL,MultiThreadedDLL>>")

# Windows-specific settings
if(NOT WIN32)
    message(STATUS "windbg_agent only builds on Windows - skipping")
    return()
endif()

# Add libagents if building standalone (not part of monorepo)
if(NOT TARGET libagents)
    add_subdirectory(external/libagents)
endif()

# Fetch cpp-httplib for handoff server and CLI
include(FetchContent)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(cpp_httplib)

# windbg_agent DLL
add_library(windbg_agent SHARED
    main.cpp
    output_capture.cpp
    settings.cpp
    session_store.cpp
    dml_output.cpp
    windbg_client.cpp
    handoff_server.cpp
    mcp_server.cpp
)

# Link with libagents and Windows debugging libraries
target_link_libraries(windbg_agent
    PRIVATE
        libagents         # Unified provider library
        fastmcpp_core     # MCP server library (via libagents/claude-agent-sdk-cpp)
        dbgeng            # Debugging Engine API
        dbghelp           # Debug Help Library
        ws2_32            # Winsock for handoff server
)

# Include httplib headers (no OpenSSL needed for localhost)
target_include_directories(windbg_agent PRIVATE ${cpp_httplib_SOURCE_DIR})

# Export symbols for WinDbg extension callbacks
target_compile_definitions(windbg_agent PRIVATE
    _WINDOWS
    UNICODE
    _UNICODE
)

# Set output name without lib prefix
set_target_properties(windbg_agent PROPERTIES
    PREFIX ""
    OUTPUT_NAME "windbg_agent"
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

# ============================================================================
# Handoff CLI executable
# ============================================================================
add_executable(windbg_agent_cli
    cli/main.cpp
    settings.cpp
)

target_include_directories(windbg_agent_cli PRIVATE
    ${cpp_httplib_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# nlohmann/json is available through libagents dependencies
target_link_libraries(windbg_agent_cli PRIVATE
    libagents
    nlohmann_json::nlohmann_json
    ws2_32
)

# Output as windbg_agent.exe
set_target_properties(windbg_agent_cli PROPERTIES
    OUTPUT_NAME "windbg_agent"
)

# Static runtime for standalone distribution
set_target_properties(windbg_agent_cli PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

