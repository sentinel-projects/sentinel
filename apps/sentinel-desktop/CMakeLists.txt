add_library(sentinel_desktop_shortcuts STATIC
    include/sentinel/desktop/GlobalShortcutBackend.h)
target_include_directories(sentinel_desktop_shortcuts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sentinel_desktop_shortcuts PUBLIC Qt6::Core)

if(TARGET Qt6::DBus)
    add_library(sentinel_desktop_portal STATIC
        include/sentinel/desktop/PortalShortcutBackend.h
        src/PortalShortcutBackend.cpp)
    target_include_directories(sentinel_desktop_portal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sentinel_desktop_portal PUBLIC sentinel_desktop_shortcuts Qt6::DBus)
    sentinel_apply_compiler_flags(sentinel_desktop_portal)
endif()

add_library(sentinel_desktop_ipc STATIC
    include/sentinel/desktop/DaemonClient.h
    src/DaemonClient.cpp
)
target_include_directories(sentinel_desktop_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/protocol
)
target_link_libraries(sentinel_desktop_ipc PUBLIC Qt6::Core Qt6::Network)
sentinel_apply_compiler_flags(sentinel_desktop_ipc)

qt_add_qml_module(sentinel_warden STATIC
    URI Sentinel.Warden VERSION 1.0 NO_PLUGIN
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Sentinel/Warden"
    SOURCES include/sentinel/desktop/CoreWardenItem.h src/CoreWardenItem.cpp)
target_include_directories(sentinel_warden PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/sentinel/desktop)
target_link_libraries(sentinel_warden PUBLIC sentinel_core Qt6::Quick Qt6::Concurrent)
sentinel_apply_compiler_flags(sentinel_warden)

add_library(sentinel_desktop_viewmodels STATIC
    include/sentinel/desktop/ExtensionsHubPresentation.h
    src/ExtensionsHubPresentation.cpp
    include/sentinel/desktop/DesktopBackupHelper.h
    src/DesktopBackupHelper.cpp
    src/DesktopMemoryArchive.cpp
    include/sentinel/desktop/QuickPanelController.h
    src/QuickPanelController.cpp
    include/sentinel/desktop/DesktopModelHelper.h
    src/RemoteAgentInspectorService.cpp
    include/sentinel/desktop/RemoteAgentInspectorService.h
    src/DesktopModelHelper.cpp
    include/sentinel/desktop/DesktopRuntimeClient.h
    include/sentinel/desktop/DesktopControllerBridge.h
    include/sentinel/desktop/DesktopErrorProjection.h
    src/DesktopRuntimeClient.cpp
    src/DesktopControllerBridge.cpp
    include/sentinel/desktop/ChatMessageListModel.h
    include/sentinel/desktop/DesktopShellViewModel.h
    include/sentinel/desktop/viewmodels/ChatViewModel.h
    include/sentinel/desktop/viewmodels/AgentInspectorViewModel.h
    src/ChatMessageListModel.cpp
    src/DesktopShellViewModel.cpp
    src/viewmodels/ChatViewModel.cpp
    src/viewmodels/AgentInspectorViewModel.cpp
)

add_library(sentinel_desktop_startup STATIC
    include/sentinel/desktop/GraphicsBackend.h
    src/GraphicsBackend.cpp
    include/sentinel/desktop/WindowStatePersistence.h
    src/WindowStatePersistence.cpp
)
target_link_libraries(sentinel_desktop_viewmodels PUBLIC sentinel_desktop_shortcuts)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(sentinel_desktop_viewmodels PUBLIC sentinel_desktop_portal)
endif()

target_include_directories(sentinel_desktop_startup
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sentinel_desktop_startup
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
)

target_include_directories(sentinel_desktop_viewmodels
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sentinel_desktop_viewmodels
    PUBLIC
        sentinel_core
        sentinel_desktop_ipc
        sentinel_warden
        Qt6::Core
        Qt6::Widgets
        Qt6::Quick
        Qt6::Concurrent
        Qt6::Network
)

qt_add_executable(sentinel-desktop
    include/sentinel/desktop/NativeCompanionAdapter.h
    bootstrap/ApplicationBootstrapper.h
    bootstrap/ApplicationBootstrapper.cpp
    bootstrap/GraphicsInitializer.h
    bootstrap/GraphicsInitializer.cpp
    bootstrap/LoggingInitializer.h
    bootstrap/LoggingInitializer.cpp
    bootstrap/PlatformInitializer.h
    bootstrap/PlatformInitializer.cpp
    bootstrap/SingleInstanceGuard.h
    bootstrap/SingleInstanceGuard.cpp
    main.cpp
    src/NativeCompanionAdapter.cpp
)

set(SENTINEL_TRANSLATION_SOURCES
    ${CMAKE_SOURCE_DIR}/translations/sentinel_en.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_tr.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_de.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_es.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_fr.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_zh.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_ja.ts
    ${CMAKE_SOURCE_DIR}/translations/sentinel_ar.ts
)

qt_add_translations(sentinel-desktop
    TS_FILES ${SENTINEL_TRANSLATION_SOURCES}
)

add_custom_target(update_translations
    COMMAND Qt6::lupdate ${CMAKE_SOURCE_DIR}/core ${CMAKE_SOURCE_DIR}/apps/sentinel-desktop ${CMAKE_SOURCE_DIR}/ui/qml -ts ${SENTINEL_TRANSLATION_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating translation .ts files with Qt lupdate"
)

set(SENTINEL_APP_ICON_PNG ${CMAKE_SOURCE_DIR}/resources/branding/app-icon/linux/512x512/apps/sentinel.png)
set(SENTINEL_APP_ICON_SVG ${CMAKE_SOURCE_DIR}/resources/branding/app-icon/linux/scalable/apps/sentinel.svg)
set(SENTINEL_APP_ICON_ICNS ${CMAKE_SOURCE_DIR}/resources/branding/app-icon/macos/sentinel.icns)
set(SENTINEL_APP_ICON_ICO ${CMAKE_SOURCE_DIR}/resources/branding/app-icon/windows/sentinel.ico)
if(APPLE)
    set(SENTINEL_TRAY_ICON ${CMAKE_SOURCE_DIR}/resources/branding/tray/macos/sentinel-tray-22-template@2x.png)
elseif(WIN32)
    set(SENTINEL_TRAY_ICON ${CMAKE_SOURCE_DIR}/resources/branding/tray/windows/sentinel-tray-32-black.png)
else()
    set(SENTINEL_TRAY_ICON ${CMAKE_SOURCE_DIR}/resources/branding/tray/linux/sentinel-tray-32-black.png)
endif()
set_source_files_properties(${SENTINEL_TRAY_ICON}
    PROPERTIES QT_RESOURCE_ALIAS branding/tray.png)
set(SENTINEL_WORDMARK ${CMAKE_SOURCE_DIR}/resources/branding/wordmark/sentinel-lockup.svg)
set_source_files_properties(${SENTINEL_WORDMARK}
    PROPERTIES QT_RESOURCE_ALIAS branding/sentinel-lockup.svg)

# Branding inputs are required; never substitute a stale or system icon.
foreach(SENTINEL_REQUIRED_BRAND_ASSET IN ITEMS
        "${SENTINEL_APP_ICON_PNG}" "${SENTINEL_APP_ICON_SVG}"
        "${SENTINEL_APP_ICON_ICNS}" "${SENTINEL_APP_ICON_ICO}"
        "${SENTINEL_TRAY_ICON}" "${SENTINEL_WORDMARK}")
    if(NOT EXISTS "${SENTINEL_REQUIRED_BRAND_ASSET}")
        message(FATAL_ERROR "Missing authoritative branding asset: ${SENTINEL_REQUIRED_BRAND_ASSET}")
    endif()
endforeach()

set(SENTINEL_WINDOWS_MANIFEST ${CMAKE_SOURCE_DIR}/packaging/windows/sentinel-desktop.exe.manifest)
set(SENTINEL_WINDOWS_RESOURCE ${CMAKE_CURRENT_BINARY_DIR}/sentinel-desktop.rc)
set(SENTINEL_MACOS_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/bundle/Info.plist.in)

set_source_files_properties(${SENTINEL_APP_ICON_PNG}
    PROPERTIES
        QT_RESOURCE_ALIAS icons/dev.sentinel.Sentinel.png
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/icons/lucide/paperclip.svg
    PROPERTIES QT_RESOURCE_ALIAS icons/lucide/paperclip.svg)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/icons/lucide/image.svg
    PROPERTIES QT_RESOURCE_ALIAS icons/lucide/image.svg)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/icons/lucide/file-text.svg
    PROPERTIES QT_RESOURCE_ALIAS icons/lucide/file-text.svg)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/icons/lucide/mic.svg
    PROPERTIES QT_RESOURCE_ALIAS icons/lucide/mic.svg)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/icons/lucide/square.svg
    PROPERTIES QT_RESOURCE_ALIAS icons/lucide/square.svg)
file(GLOB SENTINEL_TABLER_ICONS CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/resources/icons/tabler/*.svg)
foreach(SENTINEL_TABLER_ICON IN LISTS SENTINEL_TABLER_ICONS)
    get_filename_component(SENTINEL_TABLER_ICON_NAME ${SENTINEL_TABLER_ICON} NAME)
    set_source_files_properties(${SENTINEL_TABLER_ICON}
        PROPERTIES QT_RESOURCE_ALIAS icons/tabler/${SENTINEL_TABLER_ICON_NAME})
endforeach()
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/sounds/notification.wav
    PROPERTIES
        QT_RESOURCE_ALIAS sounds/notification.wav
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/sounds/error.wav
    PROPERTIES
        QT_RESOURCE_ALIAS sounds/error.wav
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/sounds/success.wav
    PROPERTIES
        QT_RESOURCE_ALIAS sounds/success.wav
)

set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/fonts/InterVariable.ttf
    PROPERTIES
        QT_RESOURCE_ALIAS fonts/Inter/Inter-Variable.ttf
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/fonts/IBMPlexMono-Regular.ttf
    PROPERTIES
        QT_RESOURCE_ALIAS fonts/IBM_Plex_Mono/IBMPlexMono-Regular.ttf
)

qt_add_resources(sentinel-desktop sentinel_desktop_assets
    PREFIX /
    FILES
        ${SENTINEL_APP_ICON_PNG}
        ${SENTINEL_TRAY_ICON}
        ${SENTINEL_WORDMARK}
        ${CMAKE_SOURCE_DIR}/resources/icons/lucide/paperclip.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/lucide/image.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/lucide/file-text.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/lucide/mic.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/lucide/square.svg
        ${SENTINEL_TABLER_ICONS}
        ${CMAKE_SOURCE_DIR}/resources/sounds/notification.wav
        ${CMAKE_SOURCE_DIR}/resources/sounds/error.wav
        ${CMAKE_SOURCE_DIR}/resources/sounds/success.wav
        ${CMAKE_SOURCE_DIR}/resources/fonts/InterVariable.ttf
        ${CMAKE_SOURCE_DIR}/resources/fonts/IBMPlexMono-Regular.ttf
)

if(APPLE)
    target_sources(sentinel-desktop PRIVATE src/MacNativeActivation.mm src/MacNativeNotifications.mm include/sentinel/desktop/MacNativeNotifications.h)
    set_source_files_properties(src/MacNativeActivation.mm src/MacNativeNotifications.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(sentinel-desktop PRIVATE "-framework Carbon" "-framework AppKit" "-framework UserNotifications")
    target_sources(sentinel-desktop PRIVATE ${SENTINEL_APP_ICON_ICNS})
    set_source_files_properties(${SENTINEL_APP_ICON_ICNS}
        PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources
    )
elseif(WIN32)
    target_link_libraries(sentinel-desktop PRIVATE user32)
endif()

if(WIN32)
    configure_file(
        ${CMAKE_SOURCE_DIR}/packaging/windows/sentinel-desktop.rc.in
        ${SENTINEL_WINDOWS_RESOURCE}
        @ONLY
    )
    target_sources(sentinel-desktop PRIVATE
        ${SENTINEL_WINDOWS_RESOURCE}
    )
endif()

set(SENTINEL_DESKTOP_QML_FILES
    ${CMAKE_SOURCE_DIR}/ui/qml/Main.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/theme/SentinelTheme.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/theme/MotionTokens.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/theme/InteractionTokens.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/Atmosphere.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/CoreWarden.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/EmptyState.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/ErrorBanner.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/GlowSurface.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/InfoRow.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/RuntimeBadge.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SectionTitle.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelButton.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelIcon.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelComboBox.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelOrb.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelSlider.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelSpinBox.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelSwitch.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelTelemetry.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SentinelTextField.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SettingCard.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SettingControlRow.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SettingToggleRow.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/ShimmerEffect.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/SoundManager.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/StatusChip.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/StatusPulse.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/base/TablerGlyph.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/chat/HomeChatSurface.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/chat/RuntimeStateStrip.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/chat/ActiveAgentsPanel.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/chat/CognitionStreamPanel.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/dialogs/ModelDetailPopup.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/dialogs/RuntimeDetailPopup.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/dialogs/SentinelOverlayModal.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/dialogs/SplashScreen.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/dialogs/TrayCompanionWindow.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/dialogs/UpdateProgressModal.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/NavigationRail.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/CommandPalette.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/HeaderBar.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/ShellPanel.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/StatusBar.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/TitleBar.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/navigation/TitleBarButton.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/notifications/NotificationCenterPanel.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/notifications/NotificationItemDelegate.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/components/notifications/NotificationToast.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/ChatPanel.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/DashboardPage.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/ExtensionsHubPage.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/MemoryPage.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/AgentInspectorPage.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/models/ModelCatalog.js
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/models/ModelsPage.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/models/LocalModelsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/models/OnlineRegistryTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/SettingsPage.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/AppearanceSettingsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/ModelSettingsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/VoiceSettingsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/WorkspaceSettingsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/SecuritySettingsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/pages/settings/SystemSettingsTab.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/OnboardingScreen.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/WelcomeStep.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/PrivacyConsentStep.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/ProcessingModeStep.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/ProviderSetupStep.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/ModelSetupStep.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/VoiceSetupStep.qml
    ${CMAKE_SOURCE_DIR}/ui/qml/onboarding/FinishStep.qml
)

sentinel_register_qml_files(
    FILES
        ${SENTINEL_DESKTOP_QML_FILES}
    SINGLETONS
        SentinelTheme.qml
        MotionTokens.qml
        InteractionTokens.qml
)

sentinel_apply_compiler_flags(sentinel_desktop_viewmodels)
sentinel_apply_compiler_flags(sentinel_desktop_startup)
sentinel_apply_compiler_flags(sentinel-desktop)

sentinel_enable_pch(sentinel_desktop_viewmodels)
sentinel_enable_pch(sentinel-desktop)


# This helper is imported explicitly by ModelsPage, not as a module-wide script.
set_source_files_properties(${CMAKE_SOURCE_DIR}/ui/qml/pages/models/ModelCatalog.js
    PROPERTIES QT_QML_SKIP_QMLDIR_ENTRY TRUE)

qt_add_qml_module(sentinel-desktop
    URI Sentinel.Desktop
    VERSION 1.0
    QML_FILES
        ${SENTINEL_DESKTOP_QML_FILES}
)

target_link_libraries(sentinel-desktop
    PRIVATE
        sentinel_desktop_viewmodels
        sentinel_desktop_startup
        sentinel_desktop_ipc
        sentinel_core
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Qml
        Qt6::Widgets
        Qt6::Multimedia
)

set_target_properties(sentinel-desktop PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "Sentinel"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.sentinel.Sentinel"
    MACOSX_BUNDLE_ICON_FILE "sentinel.icns"
    MACOSX_BUNDLE_INFO_PLIST ${SENTINEL_MACOS_INFO_PLIST}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    WIN32_EXECUTABLE TRUE
)

if(APPLE)
    set(SENTINEL_MACOS_RESOURCES
        ${SENTINEL_APP_ICON_ICNS}
        ${CMAKE_SOURCE_DIR}/packaging/macos/bundle/PrivacyInfo.xcprivacy
        ${CMAKE_SOURCE_DIR}/packaging/macos/bundle/Sentinel.sdef
        ${CMAKE_SOURCE_DIR}/packaging/macos/uninstall.sh
    )
    set_source_files_properties(${SENTINEL_MACOS_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_source_files_properties(${CMAKE_SOURCE_DIR}/packaging/macos/bundle/en.lproj/InfoPlist.strings PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/en.lproj"
    )
    set_source_files_properties(${CMAKE_SOURCE_DIR}/packaging/macos/bundle/tr.lproj/InfoPlist.strings PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/tr.lproj"
    )
    target_sources(sentinel-desktop PRIVATE
        ${SENTINEL_MACOS_RESOURCES}
        ${CMAKE_SOURCE_DIR}/packaging/macos/bundle/en.lproj/InfoPlist.strings
        ${CMAKE_SOURCE_DIR}/packaging/macos/bundle/tr.lproj/InfoPlist.strings
    )
    set_target_properties(sentinel-desktop PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/packaging/macos/bundle/sentinel.entitlements"
    )
    # Ninja/Make builds otherwise retain only the linker's executable signature,
    # which does not bind Info.plist and cannot establish notification identity.
    # Release packaging may replace this local signature with Developer ID signing.
    if(NOT CMAKE_GENERATOR MATCHES "Xcode")
        find_program(SENTINEL_CODESIGN_EXECUTABLE codesign REQUIRED)
        add_custom_command(TARGET sentinel-desktop POST_BUILD
            COMMAND ${SENTINEL_CODESIGN_EXECUTABLE} --force --sign -
                --identifier ${SENTINEL_APP_ID} $<TARGET_BUNDLE_DIR:sentinel-desktop>
            VERBATIM
        )
    endif()
endif()

if(MINGW)
    target_compile_options(sentinel-desktop PRIVATE "-Wa,-mbig-obj")
endif()

include(GNUInstallDirs)

set(SENTINEL_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}")
if(WIN32)
    # Native Windows industry standard: place main executable in the installation root
    set(SENTINEL_INSTALL_DIR ".")
endif()

install(TARGETS sentinel-desktop
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${SENTINEL_INSTALL_DIR}
)

if(WIN32)
    # Set properties for CPack WiX generator to create Start Menu and Desktop shortcuts natively
    set_target_properties(sentinel-desktop PROPERTIES
        CPACK_DESKTOP_SHORTCUTS "Sentinel"
        CPACK_START_MENU_SHORTCUTS "Sentinel"
    )

    get_target_property(_qmake_path Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_path}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    if(WINDEPLOYQT_EXECUTABLE)
        install(CODE "
            message(STATUS \"Running windeployqt on \${CMAKE_INSTALL_PREFIX}/${SENTINEL_INSTALL_DIR}/sentinel-desktop.exe\")
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" \"\${CMAKE_INSTALL_PREFIX}/${SENTINEL_INSTALL_DIR}/sentinel-desktop.exe\" --qmldir \"${CMAKE_SOURCE_DIR}/ui/qml\" --compiler-runtime
                RESULT_VARIABLE windeployqt_res
            )
            if(NOT windeployqt_res EQUAL 0)
                message(WARNING \"windeployqt failed with code \${windeployqt_res}\")
            endif()
        ")
    else()
        message(WARNING "windeployqt executable not found!")
    endif()

    # Bundle MinGW runtime DLLs (libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll)
    # These are required on systems without MinGW installed
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(_mingw_runtime_dlls
            "${_mingw_bin_dir}/libstdc++-6.dll"
            "${_mingw_bin_dir}/libgcc_s_seh-1.dll"
            "${_mingw_bin_dir}/libwinpthread-1.dll"
        )
        install(FILES ${_mingw_runtime_dlls}
            DESTINATION ${SENTINEL_INSTALL_DIR}
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/dev.sentinel.Sentinel.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    )

    install(FILES ${SENTINEL_APP_ICON_SVG}
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME dev.sentinel.Sentinel.svg
    )
    foreach(SENTINEL_ICON_SIZE 16 22 24 32 48 64 128 256 512)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/branding/app-icon/linux/${SENTINEL_ICON_SIZE}x${SENTINEL_ICON_SIZE}/apps/sentinel.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SENTINEL_ICON_SIZE}x${SENTINEL_ICON_SIZE}/apps
            RENAME dev.sentinel.Sentinel.png
        )
    endforeach()

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/dev.sentinel.Sentinel.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    )

    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )

    install(FILES ${CMAKE_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/etc/sentinel/config.json.template
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/sentinel
    )

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/systemd/sentinel-desktop.service
        DESTINATION lib/systemd/user
    )
endif()

target_sources(sentinel_desktop_viewmodels PRIVATE include/sentinel/desktop/SystemDictationService.h)
if(APPLE)
    target_sources(sentinel_desktop_viewmodels PRIVATE src/MacSystemDictationService.mm)
    set_source_files_properties(src/MacSystemDictationService.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(sentinel_desktop_viewmodels PUBLIC "-framework Speech" "-framework AVFoundation")
else()
    target_sources(sentinel_desktop_viewmodels PRIVATE src/SystemDictationService.cpp)
endif()

# Core Warden runtime assets: no Blender or preview/source resources.
file(GLOB_RECURSE SENTINEL_WARDEN_PNG CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/core-warden/animations/*.png")
set(SENTINEL_WARDEN_ASSETS ${SENTINEL_WARDEN_PNG}
    "${CMAKE_SOURCE_DIR}/assets/core-warden/animations/manifest.json"
    "${CMAKE_SOURCE_DIR}/assets/core-warden/state-contract.json")
foreach(asset IN LISTS SENTINEL_WARDEN_ASSETS)
    file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}/assets/core-warden" "${asset}")
    get_filename_component(directory "${relative}" DIRECTORY)
    if(APPLE)
        set_source_files_properties("${asset}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/core-warden/${directory}")
        target_sources(sentinel-desktop PRIVATE "${asset}")
    elseif(WIN32)
        install(FILES "${asset}" DESTINATION "core-warden/${directory}")
    else()
        install(FILES "${asset}" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/sentinel/core-warden/${directory}")
    endif()
endforeach()
