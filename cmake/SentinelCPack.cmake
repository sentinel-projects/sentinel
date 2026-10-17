# cmake/SentinelCPack.cmake
# Cross-Platform CPack Configuration for Sentinel (Windows, Linux, macOS)

set(CPACK_PACKAGE_NAME "sentinel-desktop")
set(CPACK_PACKAGE_VENDOR "Sopwit")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local-first AI desktop assistant")
set(CPACK_PACKAGE_CONTACT "sopwith.osdev@gmail.com")

configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

if(WIN32)
    set(CPACK_GENERATOR "NSIS;WIX")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Sentinel")
    set(CPACK_PACKAGE_EXECUTABLES "sentinel-desktop" "Sentinel")
    set(CPACK_CREATE_DESKTOP_LINKS "sentinel-desktop")

    # NSIS (EXE) Settings
    set(CPACK_NSIS_DISPLAY_NAME "Sentinel")
    set(CPACK_NSIS_PACKAGE_NAME "Sentinel")
    set(CPACK_NSIS_HELP_LINK "https://sentinel.dev")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://sentinel.dev")
    set(CPACK_NSIS_CONTACT "sopwith.osdev@gmail.com")
    set(CPACK_NSIS_MODIFY_PATH OFF)

    # NSIS Visuals
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/resources/branding/app-icon/windows/sentinel.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/resources/branding/app-icon/windows/sentinel.ico")
    set(CPACK_NSIS_BRANDING_TEXT "Sentinel Installer")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "sentinel-desktop.exe")

    # NSIS Shortcuts & Links (start menu entry created by CPack via CPACK_PACKAGE_EXECUTABLES)
    set(CPACK_NSIS_MENU_LINKS
        "sentinel-desktop.exe" "Sentinel"
        "https://sentinel.dev" "Sentinel Website"
    )

    # WiX uses a version-specific product identity and a stable upgrade family.
    # Keep the upgrade GUID unchanged so later MSI releases upgrade this product.
    set(CPACK_WIX_PRODUCT_GUID "A81630C2-FDAC-42E0-82D3-59B49A6EAB0C" CACHE STRING "WiX Product GUID")
    set(CPACK_WIX_UPGRADE_GUID "52FB3B44-5C09-4D74-8E40-6CEB4D5FA8B2" CACHE STRING "WiX Upgrade GUID; do not change after release")
    set(CPACK_WIX_UI_REF "WixUI_InstallDir")
    set(CPACK_WIX_LICENSE_RTF "${CMAKE_SOURCE_DIR}/packaging/windows/installer/license.rtf")
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/resources/branding/app-icon/windows/sentinel.ico")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Sentinel")
    set(CPACK_WIX_PROPERTY_ARPHELPLINK "https://sentinel.dev")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://sentinel.dev")
    set(CPACK_WIX_PROPERTY_ARPCONTACT "sopwith.osdev@gmail.com")

elseif(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libgl1, libqt6core6, libqt6gui6, libqt6qml6, libqt6quick6, libqt6widgets6, libqt6network6, libqt6sql6, libqt6sql6-sqlite, libqt6multimedia6")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Sentinel Team <sopwith.osdev@gmail.com>")
    string(REPLACE "-" "~" CPACK_RPM_PACKAGE_VERSION "${SENTINEL_APP_VERSION}")
    set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase >= 6.5.0, qt6-qtbase-gui >= 6.5.0, (qt6-qtwayland >= 6.5.0 or qt6-qtbase-gui >= 6.11.0), qt6-qtdeclarative >= 6.5.0, qt6-qtmultimedia >= 6.5.0, hicolor-icon-theme")
    set(CPACK_RPM_PACKAGE_RECOMMENDS "libnotify, xdg-utils, libsecret, avahi-tools, bubblewrap, xdg-desktop-portal")
    set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
    set(CPACK_RPM_PACKAGE_URL "https://sentinel.dev")
    set(CPACK_RPM_PACKAGE_RELEASE "1")
    set(CPACK_RPM_PACKAGE_DESCRIPTION "Local-first AI desktop assistant optimized for Fedora KDE Plasma")
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION "/usr;/usr/bin;/usr/lib;/usr/lib/systemd;/usr/lib/systemd/user;/usr/share;/usr/share/applications;/usr/share/icons;/usr/share/icons/hicolor;/usr/share/icons/hicolor/scalable;/usr/share/icons/hicolor/scalable/apps;/usr/share/icons/hicolor/1024x1024;/usr/share/icons/hicolor/1024x1024/apps;/usr/share/metainfo;/usr/share/dbus-1;/usr/share/dbus-1/services;/etc;/etc/sentinel")

elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Sentinel")
    set(CPACK_DMG_FORMAT "UDBZ")
    set(CPACK_BUNDLE_NAME "Sentinel")
    if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/apps/sentinel-desktop/sentinel-desktop.app/Contents/Info.plist")
        set(CPACK_BUNDLE_PLIST "${CMAKE_CURRENT_BINARY_DIR}/apps/sentinel-desktop/sentinel-desktop.app/Contents/Info.plist")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/packaging/macos/bundle/Info.plist.in")
        set(CPACK_BUNDLE_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/bundle/Info.plist.in")
    endif()
    set(CPACK_BUNDLE_ICON "${CMAKE_SOURCE_DIR}/resources/branding/app-icon/macos/sentinel.icns")
    set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/resources/branding/app-icon/macos/sentinel.icns")
endif()

# Preserve known source/asset notices in every platform package, including
# drag-copy installations where sibling files would be left behind.
include(GNUInstallDirs)
set(_sentinel_notice_dir "${CMAKE_INSTALL_DATAROOTDIR}/licenses/sentinel")
if(APPLE)
    set(_sentinel_notice_dir "sentinel-desktop.app/Contents/Resources/licenses")
endif()
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${_sentinel_notice_dir}")
install(FILES "${CMAKE_SOURCE_DIR}/resources/icons/tabler/LICENSE"
    DESTINATION "${_sentinel_notice_dir}/tabler")
install(FILES "${CMAKE_SOURCE_DIR}/resources/icons/lucide/LICENSE"
    "${CMAKE_SOURCE_DIR}/resources/icons/lucide/license-provenance.json"
    DESTINATION "${_sentinel_notice_dir}/lucide")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/fonts/LICENSES/"
    DESTINATION "${_sentinel_notice_dir}/fonts")

include(CPack)
