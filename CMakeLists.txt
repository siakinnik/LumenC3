# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 siakinnik
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.0")

# smart-core is a git submodule in components/smart-core
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/smart-core/CMakeLists.txt")
    message(FATAL_ERROR "components/smart-core is missing: run `git submodule update --init`")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lumenc3)
