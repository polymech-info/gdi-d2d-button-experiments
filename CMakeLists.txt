cmake_minimum_required(VERSION 3.20)

project(win32-mini-gdi
    VERSION 0.1.0
    DESCRIPTION "Reference Win32 GDI+ / D2D sandbox (no vips, no product SDK)"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "win32-mini-gdi is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(WIN32_MINI_DIST "${CMAKE_CURRENT_SOURCE_DIR}/dist")

set(THORVG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/thorvg")
if(NOT IS_DIRECTORY "${THORVG_ROOT}/src")
    message(FATAL_ERROR "ThorVG vendor missing: ${THORVG_ROOT}")
endif()
if(NOT TARGET thorvg_pm)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/thorvg_static.cmake")
endif()

set(_TABLER_FILLED "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tabler-icons/icons/filled")
if(NOT IS_DIRECTORY "${_TABLER_FILLED}")
    message(FATAL_ERROR "Tabler filled icons missing: ${_TABLER_FILLED}")
endif()
file(TO_CMAKE_PATH "${_TABLER_FILLED}" _TABL_NORM)

add_executable(pm-win32-mini-gdi WIN32
    main.cpp
    AppDefaults.cpp
    filmstrip/FilmStrip.cpp
    filmstrip/FilmStripThumb.cpp
    glowplay/GlowPlay.cpp
    glowplay/GlowHost.cpp
    glowplay/GlowTile.cpp
    glowplay/GlowTip.cpp
    glowplay/SvgIcon.cpp
)
target_include_directories(pm-win32-mini-gdi PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(pm-win32-mini-gdi PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    NOMINMAX
    TVG_STATIC=1
    "PM_TABLER_FILLED_DIR=\"${_TABL_NORM}\""
)
target_link_libraries(pm-win32-mini-gdi PRIVATE
    user32
    gdi32
    dwmapi
    gdiplus
    d2d1
    windowscodecs
    dwrite
    ole32
    advapi32
    msimg32
    thorvg_pm
)

set_target_properties(pm-win32-mini-gdi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${WIN32_MINI_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${WIN32_MINI_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${WIN32_MINI_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${WIN32_MINI_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${WIN32_MINI_DIST}"
)
