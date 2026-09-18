# Build ThorVG as a static lib (CPU + SVG + raw loaders only) for MSVC / Win32.
# Self-contained: no pixlwiz cmake helpers.
# @param THORVG_ROOT  Absolute path to vendor/thorvg

function(_tvg_write_if_changed dest content)
  set(_need TRUE)
  if(EXISTS "${dest}")
    file(READ "${dest}" _old)
    if(_old STREQUAL content)
      set(_need FALSE)
    endif()
  endif()
  if(_need)
    get_filename_component(_dir "${dest}" DIRECTORY)
    if(_dir)
      file(MAKE_DIRECTORY "${_dir}")
    endif()
    file(WRITE "${dest}" "${content}")
  endif()
endfunction()

set(_TROOT "${THORVG_ROOT}")
if(NOT IS_DIRECTORY "${_TROOT}/src")
  message(FATAL_ERROR "thorvg_static: THORVG_ROOT invalid: ${_TROOT}")
endif()

set(_GEN "${CMAKE_CURRENT_BINARY_DIR}/thorvg_config_gen")
file(MAKE_DIRECTORY "${_GEN}")
if(WIN32)
  set(_thcfg_extra "#define WIN32_LEAN_AND_MEAN 1\n")
else()
  set(_thcfg_extra "")
endif()
set(_thcfg [[
#pragma once
#define THORVG_VERSION_STRING "1.0.4"
#define THORVG_CPU_ENGINE_SUPPORT 1
#define THORVG_SVG_LOADER_SUPPORT 1
#define THORVG_FILE_IO_SUPPORT 1
]])
_tvg_write_if_changed("${_GEN}/config.h" "${_thcfg}${_thcfg_extra}")

set(_THSRC
  "${_TROOT}/src/common/tvgColor.cpp"
  "${_TROOT}/src/common/tvgCompressor.cpp"
  "${_TROOT}/src/common/tvgMath.cpp"
  "${_TROOT}/src/common/tvgStr.cpp"
  "${_TROOT}/src/renderer/tvgAccessor.cpp"
  "${_TROOT}/src/renderer/tvgAnimation.cpp"
  "${_TROOT}/src/renderer/tvgCanvas.cpp"
  "${_TROOT}/src/renderer/tvgFill.cpp"
  "${_TROOT}/src/renderer/tvgInitializer.cpp"
  "${_TROOT}/src/renderer/tvgLoaderMgr.cpp"
  "${_TROOT}/src/renderer/tvgPaint.cpp"
  "${_TROOT}/src/renderer/tvgPicture.cpp"
  "${_TROOT}/src/renderer/tvgRender.cpp"
  "${_TROOT}/src/renderer/tvgSaver.cpp"
  "${_TROOT}/src/renderer/tvgScene.cpp"
  "${_TROOT}/src/renderer/tvgShape.cpp"
  "${_TROOT}/src/renderer/tvgTaskScheduler.cpp"
  "${_TROOT}/src/renderer/tvgText.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwFill.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwImage.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwMath.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwMemPool.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwPostEffect.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwRaster.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwRenderer.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwRle.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwShape.cpp"
  "${_TROOT}/src/renderer/cpu_engine/tvgSwStroke.cpp"
  "${_TROOT}/src/loaders/svg/tvgSvgBuilder.cpp"
  "${_TROOT}/src/loaders/svg/tvgSvgCssStyle.cpp"
  "${_TROOT}/src/loaders/svg/tvgSvgLoader.cpp"
  "${_TROOT}/src/loaders/svg/tvgSvgPath.cpp"
  "${_TROOT}/src/loaders/svg/tvgSvgUtil.cpp"
  "${_TROOT}/src/loaders/svg/tvgXmlParser.cpp"
  "${_TROOT}/src/loaders/raw/tvgRawLoader.cpp"
)

add_library(thorvg_pm STATIC ${_THSRC})
target_include_directories(thorvg_pm PUBLIC
  "${_TROOT}/inc"
  "${_TROOT}/src/common"
  "${_TROOT}/src/renderer"
  "${_TROOT}/src/renderer/cpu_engine"
  "${_TROOT}/src/loaders"
  "${_TROOT}/src/loaders/svg"
  "${_TROOT}/src/loaders/raw"
  "${_GEN}"
)
if(MSVC)
  target_compile_options(thorvg_pm PRIVATE
    /W3
    /GR
    /DNOMINMAX
    /wd4267
    /wd4244
    /wd4996
  )
endif()
target_compile_definitions(thorvg_pm PRIVATE TVG_STATIC=1)
