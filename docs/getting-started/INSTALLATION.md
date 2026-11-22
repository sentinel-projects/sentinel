# Installation

The Desktop release workflow targets Linux, macOS, and Windows; this is build configuration, not proof of a currently approved published artifact. Fedora KDE Plasma is the primary target. The Rust Windows IPC client implementation cross-compiles, but native CLI/TUI connection to the Qt daemon and packaged Windows runtime remain unverified, so Windows CLI/TUI support is not certified. The TUI V4-B macOS package is for conditional local evaluation and is not cleared for public distribution.

Use only a maintainer-approved published release, when available, from [GitHub Releases](https://github.com/Sopwit/Sentinel/releases). Draft workflow output is not an approved release. The release workflow produces Linux `.deb`, `.rpm`, AppImage, and tarball artifacts; a macOS DMG; and Windows `.exe` and `.msi` installers. Verify the accompanying `SHA256SUMS.txt` before installing.

For a source build, follow [Building](../development/BUILDING.md). Local inference requires a reachable local runtime such as Ollama and an installed model. Sentinel discovers Ollama through its configured endpoint; it does not bundle Ollama or model weights.

See [Configuration](CONFIGURATION.md) for provider setup and [Data locations](../reference/DATA_LOCATIONS.md) for removal of local data.
