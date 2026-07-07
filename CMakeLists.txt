cmake_minimum_required(VERSION 3.20)

project(frp-desk LANGUAGES CXX RC)

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slint/1.16.0")
find_package(Slint 1.16 REQUIRED)
set(SLINT_STYLE fluent CACHE STRING "Slint widget style")

add_executable(frp-desk WIN32
    src/main.cpp
    src/config.cpp
    src/version_manager.cpp
    src/frpc_manager.cpp
    src/frp-desk.rc
)

slint_target_sources(frp-desk ui/app.slint)

target_compile_features(frp-desk PRIVATE cxx_std_17)
target_compile_definitions(frp-desk PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(frp-desk PRIVATE /W4 /permissive- /utf-8)
    target_link_options(frp-desk PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_link_libraries(frp-desk PRIVATE
    Slint::Slint
    comctl32
    shell32
    shlwapi
    winhttp
    ws2_32
)

add_custom_command(TARGET frp-desk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slint/1.16.0/lib/slint_cpp.dll"
        "$<TARGET_FILE_DIR:frp-desk>/slint_cpp.dll"
)
