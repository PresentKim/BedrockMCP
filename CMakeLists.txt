cmake_minimum_required(VERSION 4.3)
project(BedrockMCP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the payload does not depend on whatever runtime DLLs the game ships.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

set(FASTMCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FASTMCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTMCPP_BUILD_CLI OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fastmcpp
    GIT_REPOSITORY https://github.com/0xeb/fastmcpp.git
    GIT_TAG v3.4.7
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fastmcpp)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

add_library(conduit STATIC
    third_party/conduit/src/experimental/legacy_reload.cpp
    third_party/conduit/src/core/logger.cpp
    third_party/conduit/src/minecraft/compatibility.cpp
    third_party/conduit/src/minecraft/version.cpp
    third_party/conduit/src/platform/windows/debug_overlay.cpp
    third_party/conduit/src/platform/windows/native_debug_renderer.cpp
)
target_include_directories(conduit PUBLIC third_party/conduit/src ${minhook_SOURCE_DIR}/include)
target_link_libraries(conduit PUBLIC minhook version d3d11 dxgi)
target_compile_definitions(conduit PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX CONDUIT_DISPLAY_VERSION="v1.0.1")

add_library(bedrock_mcp SHARED
    src/payload/dllmain.cpp
    src/payload/config.cpp
    src/payload/game_window.cpp
    src/payload/input.cpp
    src/payload/feed.cpp
    src/payload/sigscan.cpp
    src/payload/capture.cpp
    src/payload/swapchain_capture.cpp
    src/payload/ui.cpp
)
target_link_libraries(bedrock_mcp PRIVATE
    conduit
    fastmcpp_core
    minhook
    d3d11 d3d12 dxgi windowscodecs runtimeobject
    ole32 oleaut32 shell32 crypt32 psapi)
target_include_directories(bedrock_mcp PRIVATE ${minhook_SOURCE_DIR}/include)
# windows.h would otherwise pull in winsock.h, which conflicts with httplib's winsock2.h.
target_compile_definitions(bedrock_mcp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(bedrock_mcp_injector src/injector/main.cpp)
target_link_libraries(bedrock_mcp_injector PRIVATE advapi32)
