add_library(sentinel_daemon_service STATIC
    service/DaemonDesktopSettings.h
    service/DaemonDesktopSettings.cpp
    service/LocalModelRuntime.h
    service/LocalModelRuntime.cpp
    service/DaemonModelHelpers.h
    service/DaemonModelHelpers.cpp
    service/DaemonIpcServer.h
    service/DaemonIpcServer.cpp
    service/DaemonService.h
    service/DaemonService.cpp
)

target_link_libraries(sentinel_daemon_service
    PUBLIC
        sentinel_core
        Qt6::Core
        Qt6::Network
        Qt6::Sql
)

target_include_directories(sentinel_daemon_service
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(sentinel-daemon
    main.cpp
)

target_link_libraries(sentinel-daemon
    PRIVATE
        sentinel_daemon_service
        sentinel_core
        Qt6::Core
        Qt6::Network
)

sentinel_apply_compiler_flags(sentinel_daemon_service)
sentinel_enable_pch(sentinel_daemon_service)
sentinel_apply_compiler_flags(sentinel-daemon)
sentinel_enable_pch(sentinel-daemon)

include(GNUInstallDirs)

set(SENTINEL_DAEMON_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}")
if(WIN32)
    set(SENTINEL_DAEMON_INSTALL_DIR ".")
elseif(APPLE)
    # A drag-to-Applications install only copies the app bundle. Keep the
    # daemon inside it so the desktop can launch it and uninstalling the app
    # also removes the helper.
    set(SENTINEL_DAEMON_INSTALL_DIR "sentinel-desktop.app/Contents/Helpers")
endif()

install(TARGETS sentinel-daemon
    RUNTIME DESTINATION ${SENTINEL_DAEMON_INSTALL_DIR}
)

if(APPLE)
    # The helper lives in Contents/Helpers and must resolve Qt from the app's
    # Frameworks directory rather than the build machine's Qt installation.
    set_target_properties(sentinel-daemon PROPERTIES
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
endif()

qt_add_resources(sentinel_daemon_service model_catalog
    PREFIX "/sentinel"
    BASE "${PROJECT_SOURCE_DIR}/core/data"
    FILES "${PROJECT_SOURCE_DIR}/core/data/model-catalog.json"
)
