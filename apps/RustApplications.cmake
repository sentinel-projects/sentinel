# Native Rust application candidate entry point. Shared with the CI matrix.
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The Rust desktop stage requires a native host toolchain.")
endif()

find_program(SENTINEL_RUSTC NAMES rustc REQUIRED)
execute_process(
    COMMAND "${SENTINEL_RUSTC}" -vV
    RESULT_VARIABLE sentinel_rustc_result
    OUTPUT_VARIABLE sentinel_rustc_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT sentinel_rustc_result EQUAL 0)
    message(FATAL_ERROR "Could not determine the Rust host target.")
endif()
string(REGEX MATCH "host: ([A-Za-z0-9_]+(-[A-Za-z0-9_]+)+)"
       sentinel_rust_host_line "${sentinel_rustc_version}")
if(NOT sentinel_rust_host_line)
    message(FATAL_ERROR "rustc did not report a host target.")
endif()
set(sentinel_rust_host "${CMAKE_MATCH_1}")
set(SENTINEL_RUST_EXPECTED_HOST "" CACHE STRING "Optional Rust host triple required by the caller")
if(SENTINEL_RUST_EXPECTED_HOST AND NOT sentinel_rust_host STREQUAL SENTINEL_RUST_EXPECTED_HOST)
    message(FATAL_ERROR "Rust host '${sentinel_rust_host}' differs from expected '${SENTINEL_RUST_EXPECTED_HOST}'.")
endif()
set(SENTINEL_RUST_STAGE "${CMAKE_BINARY_DIR}/sentinel-rust-stage"
    CACHE PATH "New directory for the unsigned Rust desktop candidate")

if(CMAKE_HOST_WIN32)
    find_program(SENTINEL_PWSH NAMES pwsh REQUIRED)
    set(sentinel_rust_stage_command
        "${SENTINEL_PWSH}" -NoProfile -NonInteractive -File
        "${CMAKE_CURRENT_LIST_DIR}/../tools/slint-desktop-prototype/stage-desktop.ps1")
elseif(CMAKE_HOST_APPLE OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(sentinel_rust_stage_command
        "${CMAKE_CURRENT_LIST_DIR}/../tools/slint-desktop-prototype/stage-desktop.sh")
else()
    message(FATAL_ERROR "No native Rust desktop staging route for this host.")
endif()

add_custom_target(sentinel-rust-stage
    COMMAND ${sentinel_rust_stage_command} "${sentinel_rust_host}" "${SENTINEL_RUST_STAGE}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    COMMENT "Build and verify the Rust desktop candidate for ${sentinel_rust_host}"
    USES_TERMINAL
    VERBATIM
)
