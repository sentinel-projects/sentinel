cmake_minimum_required(VERSION 3.24)

# Canonical human-controlled release identity. CMake's project version and
# platform package versions derive their numeric major.minor.patch prefix.
set(SENTINEL_APP_VERSION "1.0.0-rc.8" CACHE STRING "User-visible application version")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" SENTINEL_PROJECT_VERSION "${SENTINEL_APP_VERSION}")
if(NOT SENTINEL_PROJECT_VERSION)
    message(FATAL_ERROR "SENTINEL_APP_VERSION must begin with major.minor.patch.")
endif()
# Debian and RPM order pre-releases before the final release with '~'. Keep the
# user-visible version (with '-rc.N') for file names and application metadata.
string(REPLACE "-" "~" SENTINEL_PACKAGE_VERSION "${SENTINEL_APP_VERSION}")
set(SENTINEL_BUILD_NUMBER "0")
if(SENTINEL_APP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-[A-Za-z]+\\.([0-9]+)$")
    set(SENTINEL_BUILD_NUMBER "${CMAKE_MATCH_1}")
endif()

project(Sentinel
    VERSION ${SENTINEL_PROJECT_VERSION}
    DESCRIPTION "Sentinel local-first AI desktop assistant"
    LANGUAGES CXX
)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
endif()

set(SENTINEL_APP_ID "dev.sentinel.Sentinel" CACHE STRING "Reverse-DNS application id")
set(SENTINEL_DISPLAY_NAME "Sentinel")
set(SENTINEL_ORGANIZATION_NAME "Sopwit" CACHE STRING "Application organization name")
set(SENTINEL_COPYRIGHT "© 2026 Sopwit. All rights reserved." CACHE STRING "Application copyright")
set(SENTINEL_MACOS_CATEGORY "public.app-category.productivity" CACHE STRING "macOS App Store category")
set(SENTINEL_MACOS_MIN_VERSION "13.0" CACHE STRING "Minimum supported macOS version for release bundles")
set(SENTINEL_CONFIGURED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT SENTINEL_CONFIGURED_BUILD_TYPE)
    set(SENTINEL_CONFIGURED_BUILD_TYPE "unknown")
endif()

find_package(Git QUIET)
set(SENTINEL_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SENTINEL_GIT_COMMIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(SENTINEL_GIT_COMMIT_OUTPUT)
        set(SENTINEL_GIT_COMMIT "${SENTINEL_GIT_COMMIT_OUTPUT}")
    endif()
endif()

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${SENTINEL_MACOS_MIN_VERSION}" CACHE STRING "Minimum macOS deployment target")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Sql Test Network Widgets LinguistTools Multimedia Concurrent
    OPTIONAL_COMPONENTS Pdf)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.5 REQUIRED COMPONENTS DBus)
elseif(APPLE)
    find_package(Qt6 6.5 QUIET COMPONENTS DBus)
endif()

qt_standard_project_setup(REQUIRES 6.5)

# Bundle MSVC runtime DLLs on Windows (msvcp140.dll, msvcp140_1.dll, vcruntime140.dll, etc.)
# ── Engineering & Infrastructure Modules ────────────────────────────────────
include(cmake/SentinelCompilerFlags.cmake)
include(cmake/SentinelCcache.cmake)
include(cmake/SentinelPch.cmake)
include(cmake/SentinelDependencies.cmake)
include(cmake/SentinelQmlHelpers.cmake)

# ── CPack Packaging Module ──────────────────────────────────────────────────
include(cmake/SentinelCPack.cmake)

add_subdirectory(core)
add_subdirectory(plugins)
add_subdirectory(apps)

# Signing hooks attach to executable targets, so they must be included only
# after the application composition targets exist.
include(cmake/SentinelSigning.cmake)

option(SENTINEL_BUILD_TESTS "Build Sentinel unit tests" ON)
if(SENTINEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
