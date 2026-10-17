# Packaging

Packaging inputs live in `packaging/` and CPack configuration lives in `cmake/SentinelCPack.cmake`. Linux metadata and service definitions are under `packaging/linux/`; macOS bundle inputs are under `packaging/macos/bundle/`; Windows executable and installer inputs are under `packaging/windows/`. Canonical runtime icons remain in `resources/branding/app-icon/`.

The tagged release workflow builds the `package-ready` preset and currently publishes:

| Platform | Artifacts |
| --- | --- |
| Linux | DEB, RPM, AppImage, and tarball |
| macOS | DMG |
| Windows | EXE and MSI |

CPack is also configured for platform-native package generation. Flatpak and Snap files are packaging sources, not automatically published by the GitHub release workflow. See [Release process](RELEASE_PROCESS.md).

For Fedora source/RPM construction, payload checks, upgrade/removal semantics and the confirmed
Ro-ASD producer boundary, see [Fedora RPM and Ro-ASD integration](FEDORA_RPM_ROASD.md).
