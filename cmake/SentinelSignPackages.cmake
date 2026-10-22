# cmake/SignPackagesScript.cmake
# Helper script executed during 'sign_packages' target to safely sign binaries

if(NOT SIGNTOOL_EXECUTABLE OR NOT EXISTS "${SIGNTOOL_EXECUTABLE}")
    message(FATAL_ERROR "SignTool executable not found: ${SIGNTOOL_EXECUTABLE}")
endif()

file(GLOB PACKAGES "${BINARY_DIR}/sentinel-desktop-*.exe" "${BINARY_DIR}/sentinel-desktop-*.msi")

if(NOT PACKAGES)
    message(STATUS "No generated Windows installer packages found in ${BINARY_DIR} to sign.")
else()
    foreach(PKG ${PACKAGES})
        message(STATUS "Signing package: ${PKG}")
        execute_process(
            COMMAND "${SIGNTOOL_EXECUTABLE}" ${SIGN_ARGS} "${PKG}"
            RESULT_VARIABLE RES
        )
        if(NOT "${RES}" STREQUAL "0")
            message(FATAL_ERROR "Failed to sign package: ${PKG}")
        endif()
    endforeach()
endif()
