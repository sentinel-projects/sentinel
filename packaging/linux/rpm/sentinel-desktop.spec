Name:           sentinel-desktop
Version:        1.0.0~rc.8
Release:        1%{?dist}
Summary:        Local-first AI desktop assistant

License:        GPL-3.0-or-later
URL:            https://sentinel.dev
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.27
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  qt6-qtbase-devel >= 6.5.0
BuildRequires:  qt6-qtdeclarative-devel >= 6.5.0
BuildRequires:  qt6-qtmultimedia-devel >= 6.5.0
BuildRequires:  qt6-qttools-devel >= 6.5.0
BuildRequires:  desktop-file-utils
BuildRequires:  appstream
BuildRequires:  python3
# %check runs the full CTest suite, which exercises the Linux sandbox backend.
BuildRequires:  bubblewrap

Requires:       qt6-qtbase >= 6.5.0
Requires:       qt6-qtdeclarative >= 6.5.0
Requires:       qt6-qtbase-gui >= 6.5.0
Requires:       (qt6-qtwayland >= 6.5.0 or qt6-qtbase-gui >= 6.11.0)
Requires:       qt6-qtmultimedia >= 6.5.0
Requires:       hicolor-icon-theme
Recommends:     libnotify
Recommends:     xdg-utils
Recommends:     libsecret
Recommends:     avahi-tools
# Required for approved tool execution on Linux; without it the sandbox
# backend reports unavailable and launch is denied rather than unsandboxed.
Recommends:     bubblewrap

%description
Sentinel is a cross-platform, local-first AI desktop assistant application
optimized for Fedora KDE Plasma 6. It features local Ollama integration,
memory management, key-value state persistence, and native desktop shell
companionship while ensuring privacy and explicit user control.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%check
python3 tools/packaging/check_packaging_consistency.py
python3 tools/packaging/check_install_manifest.py --image "%{buildroot}" \
    --manifest "%{_vpath_builddir}/install_manifest.txt"
desktop-file-validate %{buildroot}%{_datadir}/applications/dev.sentinel.Sentinel.desktop
if command -v appstreamcli >/dev/null 2>&1; then
    appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/dev.sentinel.Sentinel.metainfo.xml
elif command -v appstream-util >/dev/null 2>&1; then
    appstream-util validate-relax --nonet %{buildroot}%{_datadir}/metainfo/dev.sentinel.Sentinel.metainfo.xml
else
    echo 'No AppStream validator is available.' >&2
    exit 1
fi
export QT_QPA_PLATFORM=offscreen
%ctest

%post
/bin/touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
%{_bindir}/update-desktop-database &>/dev/null || :

%postun
if [ $1 -eq 0 ] ; then
    /bin/touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
    %{_bindir}/gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :
    %{_bindir}/update-desktop-database &>/dev/null || :
fi

%posttrans
%{_bindir}/gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :

%files
%{_bindir}/sentinel-desktop
%{_bindir}/sentinel-daemon
%{_bindir}/sentinel-cli
%{_bindir}/sentinel-plugin-host
%{_datadir}/applications/dev.sentinel.Sentinel.desktop
%{_datadir}/icons/hicolor/scalable/apps/dev.sentinel.Sentinel.svg
%{_datadir}/icons/hicolor/16x16/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/22x22/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/24x24/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/32x32/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/48x48/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/64x64/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/128x128/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/256x256/apps/dev.sentinel.Sentinel.png
%{_datadir}/icons/hicolor/512x512/apps/dev.sentinel.Sentinel.png
%{_datadir}/metainfo/dev.sentinel.Sentinel.metainfo.xml
# Runtime assets installed by CMake outside the usual metadata paths.
%{_datadir}/sentinel
%license %{_datadir}/licenses/sentinel
%doc %{_datadir}/doc/Sentinel
# systemd user units live in <prefix>/lib/systemd/user on Fedora, not <libdir>.
%{_prefix}/lib/systemd/user/sentinel-desktop.service
%dir %{_sysconfdir}/sentinel
%config(noreplace) %{_sysconfdir}/sentinel/config.json.template

%changelog
* Sat Oct 17 2026 Sentinel Maintainers <sopwith.osdev@gmail.com> - 1.0.0~rc.8-1
- Align the package version with the canonical CMake SENTINEL_APP_VERSION (1.0.0-rc.8)
- Package sentinel-plugin-host plus the installed license, doc and core-warden assets
- Remove the inert D-Bus activation file (no session-bus registration exists)
- Recommend bubblewrap for the Linux tool-execution sandbox
- Prefer appstreamcli, falling back to appstream-util, for metadata validation
* Tue Sep 22 2026 Sentinel Maintainers <sopwith.osdev@gmail.com> - 1.0.0-1
- Ship sentinel-daemon and sentinel-cli alongside the desktop app
- Run tests headless via QT_QPA_PLATFORM=offscreen
- Recommend optional runtime helpers (libnotify, xdg-utils, libsecret, avahi-tools)
- Initial enterprise RPM package release for Fedora KDE Plasma
- FHS 3.0, XDG Base Directory, D-Bus session activation and systemd user service integration
* Thu Aug 06 2026 Sentinel Maintainers <sopwith.osdev@gmail.com> - 1.0.0-1
- Ship sentinel-daemon and sentinel-cli alongside the desktop app
- systemd user unit now launches the headless sentinel-daemon binary
