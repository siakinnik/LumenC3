# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 siakinnik
idf_component_register(
    SRCS "main.c" "effects.c" "smart_esp.c" "smart_wifi.c" "smart_ble.c"
    INCLUDE_DIRS ".")

# The public key OTA updates must be signed with: ota_signing.pub in the
# project root (64 hex digits, from `smartsign.py keygen`), or the file named
# by the LUMENC3_OTA_KEY environment variable. Without one the firmware
# takes no updates over the network (flashing over USB always works).
idf_build_get_property(project_dir PROJECT_DIR)
if(DEFINED ENV{LUMENC3_OTA_KEY})
    set(ota_key_file "$ENV{LUMENC3_OTA_KEY}")
else()
    set(ota_key_file "${project_dir}/ota_signing.pub")
endif()
set(ota_header "${CMAKE_CURRENT_BINARY_DIR}/ota_key.h")
if(EXISTS "${ota_key_file}")
    file(READ "${ota_key_file}" ota_key_hex)
    string(STRIP "${ota_key_hex}" ota_key_hex)
    if(NOT ota_key_hex MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "${ota_key_file}: expected 64 hex digits")
    endif()
    string(LENGTH "${ota_key_hex}" ota_key_len)
    if(NOT ota_key_len EQUAL 64)
        message(FATAL_ERROR "${ota_key_file}: expected 64 hex digits, got ${ota_key_len}")
    endif()
    string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1, " ota_key_bytes "${ota_key_hex}")
    file(WRITE "${ota_header}" "// generated from ${ota_key_file}\n#define OTA_SIGNING_KEY { ${ota_key_bytes}}\n")
    message(STATUS "OTA updates signed with ${ota_key_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ota_key_file}")
else()
    file(WRITE "${ota_header}" "// no ota_signing.pub: no OTA updates\n")
    message(WARNING "No ${ota_key_file}: this firmware takes no OTA updates. Create one with `smartsign.py keygen`.")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
