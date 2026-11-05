#!/usr/bin/env python3
"""Stage native builds, assemble a distribution, and inventory final signed bytes."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import plistlib
import re
import shutil
import struct
import subprocess
import sys
from xml.parsers.expat import ExpatError


ROOT = Path(__file__).resolve().parents[1]
BUNDLE = "get-icon.bundle"
MAC = f"{BUNDLE}/Contents/MacOS/get-icon"
WIN = f"{BUNDLE}/Contents/Windows64/get-icon.4DX"
RESOURCE = f"{BUNDLE}/Contents/Resources/manifest.json"
CONSTANTS = f"{BUNDLE}/Contents/Resources/constants.xlf"
PLIST = f"{BUNDLE}/Contents/Info.plist"
SIGNATURE = f"{BUNDLE}/Contents/_CodeSignature/CodeResources"
LAYOUTS = {
    "macos-universal": {MAC, RESOURCE, CONSTANTS, PLIST},
    "windows-x64": {WIN, RESOURCE, CONSTANTS},
}
BINDING = ("source_commit", "sdk_commit", "version")
SEMVER = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-((?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)


class PackageError(ValueError):
    """A failed package contract."""


def require(condition, message):
    if not condition:
        raise PackageError(message)


def portable_relative(path, root):
    """Serialize at the path boundary, including when given PureWindowsPath."""
    value = path.relative_to(root).as_posix()
    safe_path(value)
    return value


def safe_path(value):
    require(isinstance(value, str) and bool(value), "Empty or non-string file path")
    require("\\" not in value and ":" not in value, f"Non-portable path: {value!r}")
    parts = value.split("/")
    require(not PurePosixPath(value).is_absolute()
            and not PureWindowsPath(value).is_absolute(), f"Absolute path: {value!r}")
    for part in parts:
        require(part not in ("", ".", "..") and not part.endswith((".", " "))
                and not any(ord(c) < 32 or c in '<>"|?*' for c in part),
                f"Unsafe path: {value!r}")
        require(not re.fullmatch(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])",
                                 part.split(".")[0]), f"Reserved path: {value!r}")
    return value


def no_symlink_path(path):
    path = Path(os.path.abspath(path))
    for component in (path, *path.parents):
        require(not component.is_symlink(), f"Symlink forbidden: {component}")


def walk_error(error):
    raise error


def files_in(root):
    root = Path(root)
    no_symlink_path(root)
    require(root.is_dir(), f"Missing payload directory: {root}")
    result = {}
    folded = set()
    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        for name in sorted(dirs + files):
            path = Path(directory) / name
            no_symlink_path(path)
            key = portable_relative(path, root)
            require(key.casefold() not in folded, f"Case-colliding path: {key}")
            folded.add(key.casefold())
            require(path.is_dir() or path.is_file(), f"Non-regular file: {key}")
            if path.is_file():
                result[key] = path
    return dict(sorted(result.items()))


def exact_directories(root, file_paths):
    allowed = {parent.as_posix() for key in file_paths
               for parent in PurePosixPath(key).parents if parent != PurePosixPath(".")}
    for directory, dirs, _ in os.walk(root, followlinks=False, onerror=walk_error):
        for name in dirs:
            key = portable_relative(Path(directory) / name, Path(root))
            require(key in allowed, f"Unexpected directory: {key}")


def hashes(root, exclude=()):
    return {key: hashlib.sha256(path.read_bytes()).hexdigest()
            for key, path in files_in(root).items() if key not in exclude}


def run(args, cwd=None):
    return subprocess.check_output(args, cwd=ROOT if cwd is None else cwd)


def snapshot():
    source = run(["git", "rev-parse", "HEAD"]).decode().strip()
    sdk = run(["git", "rev-parse", "HEAD:4D-Plugin-SDK"]).decode().strip()
    actual_sdk = run(["git", "-C", "4D-Plugin-SDK", "rev-parse", "HEAD"]).decode().strip()
    require(actual_sdk == sdk, "SDK checkout differs from source gitlink")
    status = run(["git", "status", "--porcelain", "--untracked-files=normal",
                  "--ignore-submodules=none"])
    require(not status, "Source checkout is dirty (tracked, untracked, or submodule changes):\n"
            + status.decode("utf-8", errors="backslashreplace"))
    version_bytes = (ROOT / "VERSION").read_bytes()
    immutable_version = run(["git", "show", f"{source}:VERSION"])
    require(version_bytes == immutable_version, "VERSION bytes differ from immutable HEAD:VERSION")
    require(b"\r" not in version_bytes and version_bytes.endswith(b"\n"),
            "VERSION must use LF line endings and end with LF")
    version = version_bytes.decode("utf-8").strip()
    require(SEMVER.fullmatch(version), f"Invalid semantic version: {version!r}")
    return dict(source_commit=source, sdk_commit=sdk, version=version)


def toolchain():
    candidates = sorted((ROOT / "get-icon/build/CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    require(len(candidates) == 1, "Expected exactly one CMake C++ compiler metadata file")
    text = candidates[0].read_text(encoding="utf-8")
    values = {}
    for field in ("ID", "VERSION"):
        match = re.search(rf'set\(CMAKE_CXX_COMPILER_{field}\s+"([^"\r\n]+)"\)', text)
        require(match, f"Missing CMAKE_CXX_COMPILER_{field}")
        values[f"compiler_{field.lower()}"] = match[1]
    lines = run(["cmake", "--version"]).decode().splitlines()
    require(lines and re.fullmatch(r"cmake version \S+", lines[0]), "Invalid CMake version")
    values["cmake"] = lines[0]
    return values


def write_json(path, value):
    path.write_bytes((json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8"))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    no_symlink_path(path)
    return json.loads(Path(path).read_bytes(), object_pairs_hook=unique_object)


def validate_metadata(data, expected):
    require(isinstance(data, dict) and type(data.get("schema")) is int
            and data["schema"] == 1, "Unsupported provenance schema")
    for key in BINDING:
        require(data.get(key) == expected[key], f"{key} differs from current source snapshot")


def validate_hashes(mapping):
    require(isinstance(mapping, dict) and mapping, "Missing file inventory")
    for key, value in mapping.items():
        safe_path(key)
        require(isinstance(value, str) and re.fullmatch("[0-9a-f]{64}", value),
                f"Invalid SHA-256 for {key}")


def exact_files(actual, expected, label):
    require(set(actual) == set(expected),
            f"{label} layout mismatch; missing={sorted(set(expected) - set(actual))}, "
            f"additional={sorted(set(actual) - set(expected))}")


def verify_hashes(payload, mapping, exclude=()):
    validate_hashes(mapping)
    actual = hashes(payload, exclude)
    exact_files(actual, mapping, "Inventory")
    for key in mapping:
        require(actual[key] == mapping[key], f"SHA-256 mismatch: {key}")


def validate_pe(path):
    with path.open("rb") as binary:
        header = binary.read(64)
        require(len(header) == 64 and header[:2] == b"MZ", "Invalid Windows DOS header")
        offset = struct.unpack_from("<I", header, 60)[0]
        require(offset >= 64, "Invalid PE header offset")
        binary.seek(offset)
        pe = binary.read(26)
        require(len(pe) == 26 and pe[:4] == b"PE\0\0"
                and struct.unpack_from("<H", pe, 4)[0] == 0x8664
                and struct.unpack_from("<H", pe, 24)[0] == 0x20B,
                "Windows binary must be AMD64 PE32+")


def validate_macos(path):
    require(sys.platform == "darwin", "Mach-O validation requires native Apple tooling")
    architectures = run(["lipo", "-archs", str(path)]).decode().split()
    require(set(architectures) == {"arm64", "x86_64"}, "Mach-O must be arm64 + x86_64")
    imports = run(["otool", "-L", str(path)]).decode().splitlines()
    require(len(imports) > 1, "No Mach-O dependency metadata")
    dependencies = []
    for line in imports:
        if not line[:1].isspace():
            continue
        dependency = line.strip().split(" (", 1)[0]
        dependencies.append(dependency)
        require(dependency.startswith(("/System/Library/", "/usr/lib/")),
                f"Non-system Mach-O dependency: {dependency}")
    require(dependencies, "No Mach-O runtime dependencies found")
    commands = run(["otool", "-l", str(path)]).decode()
    require(not re.search(r"\bcmd LC_RPATH\b", commands), "Mach-O rpaths are forbidden")


def validate_plist(payload, version):
    with (payload / PLIST).open("rb") as stream:
        info = plistlib.load(stream)
    require(isinstance(info, dict), "Info.plist must be a dictionary")
    for key, value in {"CFBundlePackageType": "BNDL", "CFBundleExecutable": "get-icon",
                       "CFBundleVersion": version,
                       "CFBundleShortVersionString": version}.items():
        require(info.get(key) == value, f"Invalid Info.plist {key}; expected {value}")


def validate_input(data, expected, platform):
    validate_metadata(data, expected)
    require(set(data) == {"schema", *BINDING, "platform", "toolchain", "files"},
            "Unexpected input manifest fields")
    require(data.get("platform") == platform, f"Incorrect input platform: {platform}")
    compiler = data.get("toolchain")
    require(isinstance(compiler, dict)
            and set(compiler) == {"compiler_id", "compiler_version", "cmake"}
            and all(isinstance(v, str) and v and not any(c in v for c in "/\\\r\n")
                    for v in compiler.values()), "Invalid portable toolchain metadata")
    validate_hashes(data.get("files"))
    exact_files(data["files"], LAYOUTS[platform], platform)


def fresh_directory(path):
    no_symlink_path(path)
    require(not path.exists(), f"Output already exists: {path}")
    path.mkdir(parents=True)


def stage(platform, output):
    expected = snapshot()
    target = {"macos": "macos-universal", "windows": "windows-x64"}[platform]
    source = ROOT / "get-icon/get-icon-test/Plugins"
    source /= "get-icon.bundle" if platform == "macos" else "get-icon"
    source_files = files_in(source)
    canonical = {f"{BUNDLE}/{key}": path for key, path in source_files.items()}
    exact_files(canonical, LAYOUTS[target], target)
    exact_directories(source, source_files)
    if platform == "macos":
        validate_macos(canonical[MAC])
        validate_plist(source.parent, expected["version"])
    else:
        validate_pe(canonical[WIN])
    compiler = toolchain()
    destination = output / platform
    fresh_directory(destination)
    payload = destination / "payload"
    for key, path in canonical.items():
        dest = payload / key
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)
    data = {"schema": 1, **expected, "platform": target,
            "toolchain": compiler, "files": hashes(payload)}
    validate_input(data, expected, target)
    write_json(destination / "manifest.json", data)


def assemble(inputs, output):
    expected = snapshot()
    input_files = files_in(inputs)
    exact_files(input_files,
                {f"{p}/manifest.json" for p in ("macos", "windows")}
                | {f"macos/payload/{p}" for p in LAYOUTS["macos-universal"]}
                | {f"windows/payload/{p}" for p in LAYOUTS["windows-x64"]}, "Inputs")
    exact_directories(inputs, input_files)
    manifests = {}
    for platform, target in (("macos", "macos-universal"), ("windows", "windows-x64")):
        manifests[platform] = read_json(inputs / platform / "manifest.json")
        validate_input(manifests[platform], expected, target)
        verify_hashes(inputs / platform / "payload", manifests[platform]["files"])
    mac_payload = inputs / "macos/payload"
    win_payload = inputs / "windows/payload"
    require((mac_payload / RESOURCE).read_bytes() == (win_payload / RESOURCE).read_bytes(),
            "Resource manifest bytes differ")
    require((mac_payload / CONSTANTS).read_bytes() == (win_payload / CONSTANTS).read_bytes(),
            "Resource constants bytes differ")
    validate_plist(mac_payload, expected["version"])
    validate_pe(win_payload / WIN)
    if sys.platform == "darwin":
        validate_macos(mac_payload / MAC)
    license_bytes = run(["git", "show", f'{expected["source_commit"]}:LICENSE'])
    sdk_license = run(["git", "-C", "4D-Plugin-SDK", "show",
                       f'{expected["sdk_commit"]}:LICENSE.md'])
    fresh_directory(output)
    payload = output / "payload"
    shutil.copytree(mac_payload, payload)
    (payload / MAC).chmod(0o755)
    (payload / WIN).parent.mkdir(parents=True)
    shutil.copy2(win_payload / WIN, payload / WIN)
    (payload / "LICENSE").write_bytes(license_bytes)
    (payload / "SDK-LICENSE.md").write_bytes(sdk_license)
    write_json(payload / "BUILD-PROVENANCE.json",
               {"schema": 1, **expected, "hashes": "pre-sign", "inputs": manifests})


def build_provenance(payload):
    expected = snapshot()
    build = read_json(payload / "BUILD-PROVENANCE.json")
    validate_metadata(build, expected)
    require(set(build) == {"schema", *BINDING, "hashes", "inputs"}
            and build["hashes"] == "pre-sign", "Invalid build provenance")
    require(isinstance(build["inputs"], dict)
            and set(build["inputs"]) == {"macos", "windows"}, "Missing build inputs")
    for platform, target in (("macos", "macos-universal"), ("windows", "windows-x64")):
        validate_input(build["inputs"][platform], expected, target)
    return build


def inventory(payload, output, team=None):
    no_symlink_path(output)
    require(output.absolute() == (payload / "PROVENANCE.json").absolute(),
            "Final inventory must be payload/PROVENANCE.json")
    require(not output.exists(), f"Output already exists: {output}")
    require(team is None or re.fullmatch("[A-Z0-9]{10}", team), "Invalid Apple team ID")
    build = build_provenance(payload)
    actual = hashes(payload)
    validate_payload_layout(payload, actual)
    write_json(output, {"schema": 1, **{k: build[k] for k in BINDING},
                        "expected_team": team, "files": actual})


def validate_payload_layout(payload, actual):
    required = LAYOUTS["macos-universal"] | LAYOUTS["windows-x64"] | {
        "LICENSE", "SDK-LICENSE.md", "BUILD-PROVENANCE.json"}
    allowed = required | ({SIGNATURE} if SIGNATURE in actual else set())
    exact_files(actual, allowed, "Payload")
    exact_directories(payload, allowed)


def verify(payload, manifest):
    data = read_json(manifest)
    validate_metadata(data, snapshot())
    require(set(data) == {"schema", *BINDING, "expected_team", "files"},
            "Invalid final inventory fields")
    team = data["expected_team"]
    require(team is None or (isinstance(team, str) and re.fullmatch("[A-Z0-9]{10}", team)),
            "Invalid expected Apple team")
    build_provenance(payload)
    verify_hashes(payload, data.get("files"), exclude=("PROVENANCE.json",))
    validate_payload_layout(payload, data["files"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("stage")
    command.add_argument("--platform", required=True, choices=("macos", "windows"))
    command.add_argument("--output", type=Path, required=True)
    command = commands.add_parser("assemble")
    command.add_argument("--inputs", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)
    command = commands.add_parser("inventory")
    command.add_argument("--payload", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)
    command.add_argument("--team")
    command = commands.add_parser("verify")
    command.add_argument("--payload", type=Path, required=True)
    command.add_argument("--manifest", type=Path, required=True)
    args = vars(parser.parse_args(argv))
    action = args.pop("command")
    try:
        globals()[action](**args)
    except (PackageError, OSError, subprocess.CalledProcessError,
            UnicodeError, json.JSONDecodeError, plistlib.InvalidFileException, ExpatError) as error:
        parser.exit(1, f"package: {error}\n")


if __name__ == "__main__":
    main()
