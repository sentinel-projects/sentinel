#!/usr/bin/env python3
"""Verify local development desktop stage hashes; does not execute artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile

DIGEST = re.compile(r"[0-9a-f]{64}\Z")
TARGETS = {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu", "x86_64-apple-darwin", "aarch64-apple-darwin"}
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate manifest field")
        result[key] = value
    return result


def required_path(root, relative, limit):
    path = root
    for part in Path(relative).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError("stage contains a link in a required path")
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1 or not 0 < metadata.st_size <= limit:
        raise ValueError("missing or oversized required file")
    return path


def read_file(root, relative, limit):
    path = required_path(root, relative, limit)
    with open_regular(path, limit) as stream:
        data = stream.read(limit + 1)
        if len(data) > limit:
            raise ValueError("required file grew during inspection")
        return data


def open_regular(path, limit):
    before = path.lstat()
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        current = os.fstat(fd)
        if (not stat.S_ISREG(current.st_mode) or current.st_nlink != 1
                or not 0 < current.st_size <= limit
                or (before.st_dev, before.st_ino) != (current.st_dev, current.st_ino)):
            raise ValueError("required file changed during inspection")
        return os.fdopen(fd, "rb")
    except BaseException:
        os.close(fd)
        raise


def expected_version():
    root = Path(__file__).resolve().parents[2]
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    matches = re.findall(r'^set\(SENTINEL_APP_VERSION "([A-Za-z0-9.+-]+)"', text, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError("cannot determine canonical project version")
    return matches[0]


def verify_assets(root):
    for path in ("terminal/README.txt", "terminal/share/licenses/sentinel/LICENSE",
                 "terminal/share/licenses/sentinel/THIRD_PARTY_NOTICES.md",
                 "terminal/share/sentinel/core-warden/state-contract.json",
                 "terminal/share/sentinel/core-warden/animations/manifest.json"):
        required_path(root, path, 16 * 1024 * 1024)
    assets = root / "terminal/share/sentinel/core-warden"
    count = total = 0
    def fail_walk(error):
        raise error
    for directory, subdirs, files in os.walk(assets, followlinks=False, onerror=fail_walk):
        for name in subdirs:
            if (Path(directory) / name).is_symlink():
                raise ValueError("asset tree contains a directory link")
        for name in files:
            path = Path(directory) / name
            metadata = path.lstat()
            count += 1
            total += metadata.st_size
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1 or count > 10000 or total > 512 * 1024 * 1024:
                raise ValueError("unsafe or oversized asset tree")


def verify_signature(manifest, key_path, signature_path, openssl_path):
    if not openssl_path.is_absolute():
        raise ValueError("OpenSSL executable path must be absolute")
    executable = openssl_path.resolve(strict=True)
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError("OpenSSL executable is unavailable")
    with open_regular(key_path, 44) as stream:
        key = stream.read(45)
    with open_regular(signature_path, 64) as stream:
        signature = stream.read(65)
    if len(key) != 44 or not key.startswith(ED25519_SPKI_PREFIX) or len(signature) != 64:
        raise ValueError("expected an Ed25519 DER public key and 64-byte signature")
    # Snapshot every input once; OpenSSL 3.0 requires a seekable raw input for Ed25519.
    with tempfile.TemporaryDirectory(prefix="sentinel-stage-verify-") as directory:
        directory = Path(directory)
        for name, value in (("manifest", manifest), ("key.der", key), ("signature", signature)):
            path = directory / name
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as output:
                output.write(value)
        try:
            result = subprocess.run(
                [str(executable), "pkeyutl", "-verify", "-pubin", "-keyform", "DER", "-inkey", str(directory / "key.der"), "-rawin", "-in", str(directory / "manifest"), "-sigfile", str(directory / "signature")],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env={"OPENSSL_CONF": os.devnull}, timeout=10, check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise ValueError("signature verification timed out") from error
        if result.returncode != 0:
            raise ValueError("Ed25519 signature verification failed")
    return hashlib.sha256(key).hexdigest()


def verify(stage, version, signature_inputs=None):
    root = Path(stage)
    if root.is_symlink() or not root.is_dir():
        raise ValueError("stage must be a real directory")
    desktop_bytes = read_file(root, "desktop-manifest.json", 65536)
    fingerprint = verify_signature(desktop_bytes, *signature_inputs) if signature_inputs else None
    desktop = json.loads(desktop_bytes, object_pairs_hook=unique_object)
    expected_keys = {"schema_version", "stage", "app_version", "target", "artifacts", "cli_lockfile_sha256", "desktop_lockfile_sha256", "terminal_stage", "assets_integrity"}
    if not isinstance(desktop, dict):
        raise ValueError("invalid desktop manifest")
    schema = desktop.get("schema_version")
    if schema == 2:
        expected_keys.add("terminal_manifest_sha256")
    if (set(desktop) != expected_keys or type(schema) is not int or schema not in (1, 2)
            or desktop["stage"] != "development-desktop-evaluation" or desktop["target"] not in TARGETS
            or desktop["terminal_stage"] != "terminal" or desktop["assets_integrity"] != "not-verified"
            or desktop["app_version"] != version):
        raise ValueError("invalid desktop manifest")
    if signature_inputs and schema != 2:
        raise ValueError("signed verification requires a v2 manifest binding the terminal manifest")
    for key in ("cli_lockfile_sha256", "desktop_lockfile_sha256"):
        if not isinstance(desktop[key], str) or not DIGEST.fullmatch(desktop[key]):
            raise ValueError("invalid lockfile digest")
    terminal_bytes = read_file(root, "terminal/manifest.json", 65536)
    if schema == 2:
        expected = desktop["terminal_manifest_sha256"]
        if not isinstance(expected, str) or not DIGEST.fullmatch(expected) or hashlib.sha256(terminal_bytes).hexdigest() != expected:
            raise ValueError("terminal manifest digest mismatch")
    terminal = json.loads(terminal_bytes, object_pairs_hook=unique_object)
    terminal_keys = {"schema_version", "app_version", "target", "stage", "artifacts", "lockfile_sha256", "assets_integrity"}
    if (not isinstance(terminal, dict) or set(terminal) != terminal_keys
            or type(terminal.get("schema_version")) is not int or terminal.get("schema_version") != 1 or terminal.get("stage") != "development-evaluation"
            or terminal.get("target") != desktop["target"] or terminal.get("app_version") != desktop["app_version"]
            or terminal.get("lockfile_sha256") != desktop["cli_lockfile_sha256"]
            or terminal.get("assets_integrity") != "not-verified"):
        raise ValueError("terminal stage identity mismatch")
    for manifest, prefix, expected in (
        (desktop, "", {"bin/sentinel-dev-launch", "bin/sentinel-slint-prototype"}),
        (terminal, "terminal/", {"bin/sentinel", "bin/sentinel-daemon-dev"}),
    ):
        items = manifest.get("artifacts")
        if not isinstance(items, list) or len(items) != len(expected):
            raise ValueError("invalid artifact inventory")
        remaining_paths = expected.copy()
        for item in items:
            if (not isinstance(item, dict) or set(item) != {"path", "sha256"} or item["path"] not in remaining_paths
                    or not isinstance(item["sha256"], str) or not DIGEST.fullmatch(item["sha256"])):
                raise ValueError("invalid artifact descriptor")
            remaining_paths.remove(item["path"])
            path = required_path(root, prefix + item["path"], 512 * 1024 * 1024)
            if not path.stat().st_mode & 0o111:
                raise ValueError("required artifact is not executable")
            with open_regular(path, 512 * 1024 * 1024) as stream:
                initial = os.fstat(stream.fileno())
                digest = hashlib.sha256()
                remaining_bytes = initial.st_size
                while remaining_bytes:
                    chunk = stream.read(min(1024 * 1024, remaining_bytes))
                    if not chunk:
                        raise ValueError("artifact changed during inspection")
                    digest.update(chunk)
                    remaining_bytes -= len(chunk)
                after = os.fstat(stream.fileno())
                if (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (initial.st_size, initial.st_mtime_ns, initial.st_ctime_ns):
                    raise ValueError("artifact changed during inspection")
                actual = digest.hexdigest()
            if actual != item["sha256"]:
                raise ValueError("artifact digest mismatch")
    verify_assets(root)
    return desktop, fingerprint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage")
    parser.add_argument("--expected-version", help="Expected application version (default: this checkout's canonical version)")
    parser.add_argument("--cli-lockfile", type=Path, help="Optional expected CLI Cargo.lock")
    parser.add_argument("--desktop-lockfile", type=Path, help="Optional expected desktop Cargo.lock")
    parser.add_argument("--trusted-key", type=Path, help="Pinned Ed25519 public key in DER SubjectPublicKeyInfo form")
    parser.add_argument("--signature", type=Path, help="Detached 64-byte signature over exact desktop-manifest.json bytes")
    parser.add_argument("--openssl", type=Path, help="Absolute path to trusted OpenSSL 3.x or later")
    args = parser.parse_args()
    try:
        version = args.expected_version if args.expected_version is not None else expected_version()
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?", version):
            raise ValueError("invalid expected version")
        supplied = (args.trusted_key, args.signature, args.openssl)
        if any(value is not None for value in supplied) and not all(value is not None for value in supplied):
            raise ValueError("--trusted-key, --signature and --openssl must be supplied together")
        data, fingerprint = verify(args.stage, version, supplied if all(value is not None for value in supplied) else None)
        for path, key in ((args.cli_lockfile, "cli_lockfile_sha256"), (args.desktop_lockfile, "desktop_lockfile_sha256")):
            if path is not None:
                with open_regular(path, 64 * 1024 * 1024) as stream:
                    if hashlib.file_digest(stream, "sha256").hexdigest() != data[key]:
                        raise ValueError("lockfile digest mismatch")
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(2, str(error) + "\n")
    if fingerprint:
        print(f"Development desktop stage signature and executable hashes verified for {data['target']} with supplied key SHA-256 {fingerprint}; assets and native acceptance remain unverified.")
    else:
        print(f"Development desktop stage verified for {data['target']}; hashes do not establish authenticity, asset integrity or native acceptance.")


if __name__ == "__main__":
    main()
