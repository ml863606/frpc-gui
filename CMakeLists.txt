cmake_minimum_required(VERSION 3.20)

project(frp-desk LANGUAGES CXX RC)

add_executable(frp-desk WIN32
    src/main.cpp
    src/config.cpp
    src/version_manager.cpp
    src/frpc_manager.cpp
    src/frp-desk.rc
)

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
    comctl32
    shell32
    shlwapi
    winhttp
)
