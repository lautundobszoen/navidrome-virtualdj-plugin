cmake_minimum_required(VERSION 3.21)
project(NavidromeVirtualDJ LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Dieses Projekt erzeugt ein Windows-VirtualDJ-Plugin und muss unter Windows/MSVC gebaut werden.")
endif()

add_library(Navidrome SHARED
    src/navidrome_vdj.cpp
)

target_compile_features(Navidrome PRIVATE cxx_std_17)
target_include_directories(Navidrome PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)
target_compile_definitions(Navidrome PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(Navidrome PRIVATE winhttp credui advapi32 bcrypt crypt32)
set_target_properties(Navidrome PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Navidrome"
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(Navidrome PRIVATE /W4 /permissive- /EHsc /utf-8)
    set_property(TARGET Navidrome PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
