cmake_minimum_required(VERSION 3.21)

project(frpc-gui LANGUAGES CXX RC)

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slint/1.16.0")
find_package(Slint 1.16 REQUIRED)
set(SLINT_STYLE fluent CACHE STRING "Slint widget style")

add_executable(frpc-gui WIN32
    src/main.cpp
    src/app_update.cpp
    src/config.cpp
    src/version_manager.cpp
    src/frpc_manager.cpp
    src/frpc-gui.rc
)

slint_target_sources(frpc-gui ui/app.slint)

target_compile_features(frpc-gui PRIVATE cxx_std_17)
target_compile_definitions(frpc-gui PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(frpc-gui PRIVATE /W4 /permissive- /utf-8)
    target_link_options(frpc-gui PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_link_libraries(frpc-gui PRIVATE
    Slint::Slint
    comctl32
    shell32
    shlwapi
    winhttp
    ws2_32
)

add_custom_command(TARGET frpc-gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slint/1.16.0/lib/slint_cpp.dll"
        "$<TARGET_FILE_DIR:frpc-gui>/slint_cpp.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:frpc-gui>
        "$<TARGET_FILE_DIR:frpc-gui>"
    COMMAND_EXPAND_LISTS
)
