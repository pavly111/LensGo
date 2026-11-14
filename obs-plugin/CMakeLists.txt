cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${CMAKE_SOURCE_DIR}/.deps/obs-deps-2025-07-11-universal/include")
find_library(FFMPEG_AVCODEC avcodec HINTS "${CMAKE_SOURCE_DIR}/.deps/obs-deps-2025-07-11-universal/lib")
find_library(FFMPEG_AVUTIL avutil HINTS "${CMAKE_SOURCE_DIR}/.deps/obs-deps-2025-07-11-universal/lib")
if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVCODEC OR NOT FFMPEG_AVUTIL)
  message(FATAL_ERROR "FFmpeg was not found in the OBS dependencies")
endif()
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_AVCODEC} ${FFMPEG_AVUTIL})
