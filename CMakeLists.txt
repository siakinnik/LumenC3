# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 siakinnik
cmake_minimum_required(VERSION 3.16)

# release builds pass the version from the git tag: idf.py -DPROJECT_VER=2.1.0 build
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "2.0.0")
endif()

# smart-core is a git submodule in components/smart-core. To work on the core
# and the firmware together, build against another checkout instead:
#   SMART_CORE_DIR=../smart-core idf.py build
if(DEFINED ENV{SMART_CORE_DIR})
    get_filename_component(smart_core_dir "$ENV{SMART_CORE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    message(STATUS "smart-core from ${smart_core_dir} (SMART_CORE_DIR), not the submodule")
    # COMPONENT_DIRS replaces the whole search, so ESP-IDF's own components go first
    set(COMPONENT_DIRS "$ENV{IDF_PATH}/components" "${CMAKE_CURRENT_LIST_DIR}/main" "${smart_core_dir}")
elseif(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/smart-core/CMakeLists.txt")
    message(FATAL_ERROR "components/smart-core is missing: run `git submodule update --init`")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lumenc3)
