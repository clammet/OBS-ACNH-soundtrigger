cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()
project(obs_soundtrigger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(soundtrigger-core STATIC src/detector.cpp)
target_include_directories(soundtrigger-core PUBLIC include)
target_compile_options(soundtrigger-core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(soundtrigger-replay src/replay.cpp)
target_link_libraries(soundtrigger-replay PRIVATE soundtrigger-core)

include(CTest)
if(BUILD_TESTING)
    add_executable(soundtrigger-tests tests/core.cpp)
    target_link_libraries(soundtrigger-tests PRIVATE soundtrigger-core)
    add_test(NAME core COMMAND soundtrigger-tests)
endif()

option(BUILD_OBS_PLUGIN "Build the native macOS OBS plugin" ${APPLE})
if(BUILD_OBS_PLUGIN)
    if(NOT APPLE)
        message(FATAL_ERROR "The plugin UI currently supports macOS only; use -DBUILD_OBS_PLUGIN=OFF")
    endif()
    enable_language(OBJCXX)
    set(OBS_APP "$ENV{OBS_APP}" CACHE PATH "Installed OBS.app")
    if(NOT OBS_APP)
        find_program(OBS_EXECUTABLE NAMES obs PATHS ${CMAKE_SYSTEM_APPBUNDLE_PATH}
            "$ENV{HOME}/Applications" PATH_SUFFIXES OBS.app/Contents/MacOS NO_DEFAULT_PATH)
        if(OBS_EXECUTABLE)
            get_filename_component(OBS_APP "${OBS_EXECUTABLE}/../../.." ABSOLUTE)
        endif()
    endif()
    set(OBS_SOURCE_DIR "" CACHE PATH "Official obs-studio source matching installed OBS")
    if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
        message(FATAL_ERROR "Set OBS_SOURCE_DIR to the matching OBS source tree (see README)")
    endif()
    find_library(OBS_LIBRARY NAMES libobs PATHS "${OBS_APP}/Contents/Frameworks" NO_DEFAULT_PATH REQUIRED)
    find_library(OBS_FRONTEND_LIBRARY NAMES obs-frontend-api obs-frontend-api.dylib
        PATHS "${OBS_APP}/Contents/Frameworks" NO_DEFAULT_PATH REQUIRED)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(UTI_LIBRARY UniformTypeIdentifiers REQUIRED)
    find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h REQUIRED)
    # Public headers include a generated file, but no OBS build is needed.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obs-headers/obsconfig.h"
        "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
    add_library(obs-soundtrigger MODULE src/plugin.mm)
    target_include_directories(obs-soundtrigger PRIVATE
        "${OBS_SOURCE_DIR}/libobs" "${OBS_SOURCE_DIR}/frontend/api"
        "${CMAKE_CURRENT_BINARY_DIR}/obs-headers" "${SIMDE_INCLUDE_DIR}")
    target_compile_options(obs-soundtrigger PRIVATE -fobjc-arc -Wall -Wextra)
    target_link_libraries(obs-soundtrigger PRIVATE soundtrigger-core
        "${OBS_LIBRARY}" "${OBS_FRONTEND_LIBRARY}" "${COCOA_LIBRARY}" "${UTI_LIBRARY}")
    set_target_properties(obs-soundtrigger PROPERTIES
        BUNDLE TRUE PREFIX "" BUNDLE_EXTENSION plugin
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks")
    set_property(TARGET obs-soundtrigger PROPERTY XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/acnh.stmodel")
        set_property(TARGET obs-soundtrigger APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/models/acnh.stmodel")
        add_custom_command(TARGET obs-soundtrigger POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:obs-soundtrigger>/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/models/acnh.stmodel"
                "$<TARGET_BUNDLE_DIR:obs-soundtrigger>/Contents/Resources/acnh.stmodel"
            VERBATIM)
    endif()
    add_custom_command(TARGET obs-soundtrigger POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:obs-soundtrigger>/Contents/Resources"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:obs-soundtrigger>"
        VERBATIM)
endif()
