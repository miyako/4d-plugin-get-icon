"""Portable packaging contract tests; native-tool calls are explicitly mocked."""

from contextlib import ExitStack
import io
import os
from pathlib import Path, PureWindowsPath
import plistlib
import shutil
import stat
import struct
import subprocess
import unittest
from unittest.mock import Mock, patch
import uuid

import package


def remove_readonly(function, path, exc_info):
    """Retry only a read-only fixture entry, without following symlinks."""
    error = exc_info[1]
    entry = Path(path)
    if not isinstance(error, PermissionError) or entry.is_symlink():
        raise error
    mode = entry.stat().st_mode
    if mode & stat.S_IWUSR:
        raise error
    entry.chmod(stat.S_IMODE(mode) | stat.S_IWUSR)
    function(path)


def remove_fixture(path):
    shutil.rmtree(path, onerror=remove_readonly)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parent / (".test-package-" + uuid.uuid4().hex)
        self.root.mkdir()
        self.addCleanup(remove_fixture, self.root)
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(package, "ROOT", self.root))
        self.binding = dict(source_commit="a" * 40, sdk_commit="b" * 40, version="1.2.3")
        self.stack.enter_context(patch.object(package, "snapshot", return_value=self.binding))
        self.stack.enter_context(patch.object(package, "toolchain", return_value={
            "compiler_id": "Clang", "compiler_version": "17.0", "cmake": "cmake version 3.30.0"}))
        self.native = self.stack.enter_context(patch.object(package, "validate_macos"))
        self.stack.enter_context(patch.object(package, "run", side_effect=self.export))
        self.inputs = self.root / "inputs"
        self.output = self.root / "assembled"
        self.plugins = self.root / "get-icon/get-icon-test/Plugins"
        mac = self.plugins / "get-icon.bundle/Contents"
        (mac / "MacOS").mkdir(parents=True)
        (mac / "MacOS/get-icon").write_bytes(b"fixture Mach-O; native tools mocked")
        (mac / "MacOS/get-icon").chmod(0o755)
        (mac / "Resources").mkdir()
        (mac / "Resources/manifest.json").write_bytes(b'{"name":"get-icon"}\n')
        (mac / "Resources/constants.xlf").write_bytes(b'<xliff version="1.0"/>\n')
        (mac / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundlePackageType": "BNDL", "CFBundleExecutable": "get-icon",
            "CFBundleVersion": "1.2.3", "CFBundleShortVersionString": "1.2.3"}))
        win = self.plugins / "get-icon/Contents"
        (win / "Windows64").mkdir(parents=True)
        (win / "Resources").mkdir()
        pe = bytearray(90)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 60, 64)
        pe[64:68] = b"PE\0\0"
        struct.pack_into("<H", pe, 68, 0x8664)
        struct.pack_into("<H", pe, 88, 0x20B)
        (win / "Windows64/get-icon.4DX").write_bytes(pe)
        (win / "Resources/manifest.json").write_bytes(b'{"name":"get-icon"}\n')
        (win / "Resources/constants.xlf").write_bytes(b'<xliff version="1.0"/>\n')
        package.stage("macos", self.inputs)
        package.stage("windows", self.inputs)

    def export(self, args, cwd=None):
        return b"upstream SDK license\n" if "-C" in args else b"plugin license\n"

    def manifest(self, platform="windows"):
        return self.inputs / platform / "manifest.json"

    def update_manifest(self, change, platform="windows"):
        path = self.manifest(platform)
        data = package.read_json(path)
        change(data)
        package.write_json(path, data)

    def assemble(self):
        package.assemble(self.inputs, self.output)
        return self.output / "payload"

    def test_roundtrip_and_pre_sign_hashes(self):
        payload = self.assemble()
        binary = payload / package.MAC
        before = package.hashes(payload)[package.MAC]
        binary.write_bytes(binary.read_bytes() + b"signed")
        signature = payload / package.SIGNATURE
        signature.parent.mkdir()
        signature.write_bytes(b"signature")
        final = payload / "PROVENANCE.json"
        package.inventory(payload, final, "ABCDEFGHIJ")
        package.verify(payload, final)
        data = package.read_json(final)
        self.assertNotEqual(data["files"][package.MAC], before)
        self.assertEqual(data["expected_team"], "ABCDEFGHIJ")
        build = package.read_json(payload / "BUILD-PROVENANCE.json")
        self.assertEqual(build["inputs"]["macos"], package.read_json(self.manifest("macos")))
        self.assertEqual(build["inputs"]["macos"]["files"][package.MAC], before)
        self.assertEqual(build["hashes"], "pre-sign")
        self.assertTrue(final.read_bytes().endswith(b"\n"))
        self.assertNotIn(b"\r", final.read_bytes())

    def test_artifact_handoff_restores_executable_permission(self):
        source = self.inputs / "macos/payload" / package.MAC
        source.chmod(0o644)
        original_hash = package.hashes(self.inputs / "macos/payload")[package.MAC]
        chmod = Path.chmod
        requests = []

        def record_chmod(path, mode, *args, **kwargs):
            requests.append((path, mode))
            return chmod(path, mode, *args, **kwargs)

        with patch.object(Path, "chmod", new=record_chmod):
            payload = self.assemble()
        binary = payload / package.MAC
        self.assertIn((binary, 0o755), requests)
        if os.name != "nt":
            self.assertEqual(binary.stat().st_mode & 0o777, 0o755)
        self.assertEqual(package.hashes(payload)[package.MAC], original_hash)

    def test_windows_path_boundary(self):
        root = PureWindowsPath(r"C:\artifact\payload")
        self.assertEqual(package.portable_relative(root / r"get-icon.bundle\Contents\Windows64\get-icon.4DX",
                                                  root), package.WIN)

    def test_unsafe_manifest_paths(self):
        for key in ("../escape", "/absolute", r"C:\native\file", "a\\b", "a//b",
                    "a/./b", "a/../b", "NUL.txt", "a/b.", "a/b "):
            with self.subTest(key=key), self.assertRaises(package.PackageError):
                package.safe_path(key)

    def test_mixed_source_sdk_version(self):
        original = self.manifest().read_bytes()
        for field in package.BINDING:
            with self.subTest(field=field):
                self.manifest().write_bytes(original)
                self.update_manifest(lambda m: m.update({field: "different"}))
                with self.assertRaisesRegex(package.PackageError, field):
                    self.assemble()

    def test_current_snapshot_binding(self):
        for field in package.BINDING:
            with self.subTest(field=field):
                binding = {**self.binding, field: "different"}
                with patch.object(package, "snapshot", return_value=binding):
                    with self.assertRaisesRegex(package.PackageError, field):
                        self.assemble()

    def test_input_tamper_missing_additional(self):
        path = self.inputs / "windows/payload" / package.WIN
        original = path.read_bytes()
        path.write_bytes(original + b"tampered")
        with self.assertRaisesRegex(package.PackageError, "SHA-256"):
            self.assemble()
        path.unlink()
        with self.assertRaisesRegex(package.PackageError, "layout mismatch"):
            self.assemble()
        path.write_bytes(original)
        extra = path.parent / "extra.dll"
        extra.write_bytes(b"extra")
        with self.assertRaisesRegex(package.PackageError, "additional"):
            self.assemble()

    def test_final_tamper_missing_additional(self):
        payload = self.assemble()
        final = payload / "PROVENANCE.json"
        package.inventory(payload, final)
        path = payload / package.WIN
        original = path.read_bytes()
        path.write_bytes(original + b"tampered")
        with self.assertRaisesRegex(package.PackageError, "SHA-256"):
            package.verify(payload, final)
        path.unlink()
        with self.assertRaisesRegex(package.PackageError, "missing"):
            package.verify(payload, final)
        path.write_bytes(original)
        (payload / "extra").write_bytes(b"extra")
        with self.assertRaisesRegex(package.PackageError, "additional"):
            package.verify(payload, final)

    def test_symlink_rejected(self):
        link = self.inputs / "macos/payload/link"
        try:
            link.symlink_to(self.manifest())
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows account lacks symlink privilege")
            raise
        with self.assertRaisesRegex(package.PackageError, "Symlink"):
            self.assemble()

    def test_resource_mismatch_with_valid_hash(self):
        path = self.inputs / "windows/payload" / package.RESOURCE
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.update_manifest(lambda m: m.update(files=package.hashes(self.inputs / "windows/payload")))
        with self.assertRaisesRegex(package.PackageError, "Resource manifest"):
            self.assemble()

    def test_constants_mismatch_with_valid_hash(self):
        path = self.inputs / "windows/payload" / package.CONSTANTS
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.update_manifest(lambda m: m.update(files=package.hashes(self.inputs / "windows/payload")))
        with self.assertRaisesRegex(package.PackageError, "Resource constants"):
            self.assemble()

    def test_overwrite_prevention(self):
        with self.assertRaisesRegex(package.PackageError, "already exists"):
            package.stage("windows", self.inputs)
        payload = self.assemble()
        with self.assertRaisesRegex(package.PackageError, "already exists"):
            self.assemble()
        final = payload / "PROVENANCE.json"
        package.inventory(payload, final)
        with self.assertRaisesRegex(package.PackageError, "already exists"):
            package.inventory(payload, final)

    def test_exact_input_layout_even_if_inventory_updated(self):
        for platform, key in (("macos", package.MAC), ("windows", package.WIN)):
            with self.subTest(platform=platform):
                old = self.manifest(platform).read_bytes()
                self.update_manifest(lambda m: m["files"].pop(key), platform)
                with self.assertRaisesRegex(package.PackageError, "layout mismatch"):
                    self.assemble()
                self.manifest(platform).write_bytes(old)
        extra = self.inputs / "macos/payload/get-icon.bundle/Contents/Frameworks/extra.dylib"
        extra.parent.mkdir()
        extra.write_bytes(b"extra")
        self.update_manifest(lambda m: m.update(files=package.hashes(self.inputs / "macos/payload")),
                             "macos")
        with self.assertRaisesRegex(package.PackageError, "layout mismatch"):
            self.assemble()

    def test_stage_rejects_missing_platform_files(self):
        for platform, path in (
                ("macos", self.plugins / "get-icon.bundle/Contents/MacOS/get-icon"),
                ("windows", self.plugins / "get-icon/Contents/Windows64/get-icon.4DX")):
            with self.subTest(platform=platform):
                original = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(package.PackageError, "missing"):
                    package.stage(platform, self.root / ("missing-" + platform))
                path.write_bytes(original)

    def test_unexpected_empty_directory(self):
        (self.inputs / "macos/payload/unexpected").mkdir()
        with self.assertRaisesRegex(package.PackageError, "Unexpected directory"):
            self.assemble()

    def test_inventory_rejects_missing_platform(self):
        payload = self.assemble()
        (payload / package.WIN).unlink()
        with self.assertRaisesRegex(package.PackageError, "missing"):
            package.inventory(payload, payload / "PROVENANCE.json")

    def test_cli_failure_is_nonzero_and_clear(self):
        with patch("sys.stderr", new_callable=io.StringIO) as error:
            with self.assertRaises(SystemExit) as result:
                package.main(["verify", "--payload", str(self.root / "missing"),
                              "--manifest", str(self.root / "missing.json")])
        self.assertEqual(result.exception.code, 1)
        self.assertIn("package:", error.getvalue())

    def test_plist_version_and_package_type(self):
        path = self.inputs / "macos/payload" / package.PLIST
        original = path.read_bytes()
        for key in ("CFBundleVersion", "CFBundleShortVersionString",
                    "CFBundlePackageType", "CFBundleExecutable"):
            with self.subTest(key=key):
                data = plistlib.loads(original)
                data[key] = "wrong"
                path.write_bytes(plistlib.dumps(data))
                self.update_manifest(lambda m: m.update(
                    files=package.hashes(self.inputs / "macos/payload")), "macos")
                with self.assertRaisesRegex(package.PackageError, key):
                    self.assemble()

    def test_windows_architecture(self):
        path = self.inputs / "windows/payload" / package.WIN
        data = bytearray(path.read_bytes())
        struct.pack_into("<H", data, 68, 0xAA64)
        path.write_bytes(data)
        self.update_manifest(lambda m: m.update(files=package.hashes(self.inputs / "windows/payload")))
        with self.assertRaisesRegex(package.PackageError, "AMD64"):
            self.assemble()

    def test_immutable_license_bytes_from_real_git(self):
        repo = self.root / "license-repo"
        repo.mkdir()
        def git(*args):
            return subprocess.check_output(["git", *args], cwd=repo, stderr=subprocess.PIPE)
        git("init", "--quiet")
        git("config", "user.name", "Package Test")
        git("config", "user.email", "package@example.invalid")
        git("config", "core.autocrlf", "true")
        git("config", "core.eol", "crlf")
        license_bytes = b"license line one\nlicense line two\n"
        (repo / "LICENSE").write_bytes(license_bytes)
        (repo / "LICENSE.md").write_bytes(license_bytes)
        git("add", "LICENSE", "LICENSE.md")
        git("commit", "--quiet", "-m", "fixture")
        (repo / "LICENSE").unlink()
        (repo / "LICENSE.md").unlink()
        git("checkout", "--", "LICENSE", "LICENSE.md")
        self.assertIn(b"\r\n", (repo / "LICENSE").read_bytes())
        def export(args, cwd=None):
            name = "LICENSE.md" if "-C" in args else "LICENSE"
            return git("show", f"HEAD:{name}")
        with patch.object(package, "run", side_effect=export):
            payload = self.assemble()
        self.assertEqual((payload / "LICENSE").read_bytes(), license_bytes)
        self.assertEqual((payload / "SDK-LICENSE.md").read_bytes(), license_bytes)


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Path(__file__).resolve().parent / (".test-snapshot-" + uuid.uuid4().hex)
        self.fixture.mkdir()
        self.addCleanup(remove_fixture, self.fixture)
        global_config = self.fixture / "global.gitconfig"
        global_config.write_bytes(b"[core]\n\tautocrlf = true\n\teol = crlf\n")
        global_patch = patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(global_config)})
        global_patch.start()
        self.addCleanup(global_patch.stop)
        self.repo = self.fixture / "repo"
        origin = self.fixture / "sdk-origin"
        self.repo.mkdir()
        origin.mkdir()
        for directory in (self.repo, origin):
            self.git(directory, "init", "--quiet")
            self.git(directory, "config", "user.name", "Package Test")
            self.git(directory, "config", "user.email", "package@example.invalid")
            self.git(directory, "config", "core.autocrlf", "false")
        (origin / "LICENSE.md").write_bytes(b"SDK license\n")
        self.git(origin, "add", ".")
        self.git(origin, "commit", "--quiet", "-m", "SDK fixture")
        # Configure before cloning: changing autocrlf after checkout leaves CRLF
        # files dirty when the SDK fixture's immutable blobs contain LF.
        self.git(self.repo, "-c", "protocol.file.allow=always", "-c", "core.autocrlf=false",
                 "submodule", "add", "--quiet", str(origin), "4D-Plugin-SDK")
        self.sdk = self.repo / "4D-Plugin-SDK"
        self.git(self.sdk, "config", "user.name", "Package Test")
        self.git(self.sdk, "config", "user.email", "package@example.invalid")
        self.git(self.sdk, "config", "core.autocrlf", "false")
        (self.repo / "VERSION").write_bytes(b"1.2.3\n")
        (self.repo / "source.cpp").write_bytes(b"source\n")
        (self.repo / ".gitattributes").write_bytes(b"VERSION text eol=lf\n")
        (self.repo / ".gitignore").write_bytes(
            b".package-inputs/\nbuild*/\n.local-tools/\nscripts/__pycache__/\n")
        self.git(self.repo, "add", ".")
        self.git(self.repo, "commit", "--quiet", "-m", "source fixture")
        self.root_patch = patch.object(package, "ROOT", self.repo)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def git(self, directory, *args):
        return subprocess.check_output(["git", *args], cwd=directory, stderr=subprocess.PIPE)

    def test_clean_snapshot_and_ignored_outputs(self):
        self.assertEqual(self.git(self.repo, "config", "--global", "core.autocrlf").strip(), b"true")
        self.assertEqual((self.sdk / "LICENSE.md").read_bytes(), b"SDK license\n")
        self.assertEqual(self.git(self.sdk, "status", "--porcelain",
                                  "--untracked-files=normal"), b"")
        for name in (".package-inputs", "build-universal", ".local-tools", "scripts/__pycache__"):
            directory = self.repo / name
            directory.mkdir(parents=True)
            (directory / "output").write_bytes(b"ignored output")
        data = package.snapshot()
        self.assertEqual(data["source_commit"], self.git(self.repo, "rev-parse", "HEAD").decode().strip())
        self.assertEqual(data["sdk_commit"], self.git(self.sdk, "rev-parse", "HEAD").decode().strip())
        self.assertEqual(data["version"], "1.2.3")

    def test_dirty_tracked_source(self):
        (self.repo / "source.cpp").write_bytes(b"modified source\n")
        with self.assertRaisesRegex(package.PackageError, "Source checkout is dirty"):
            package.snapshot()
        self.git(self.repo, "add", "source.cpp")
        with self.assertRaisesRegex(package.PackageError, "Source checkout is dirty"):
            package.snapshot()

    def test_inherited_crlf_control_reproduces_dirty_sdk(self):
        control = self.repo / ".package-inputs/crlf-control"
        control.parent.mkdir()
        self.git(self.repo, "-c", "protocol.file.allow=always", "-c", "core.autocrlf=true",
                 "clone", "--quiet", str(self.fixture / "sdk-origin"), str(control))
        self.assertIn(b"\r\n", (control / "LICENSE.md").read_bytes())
        self.git(control, "config", "core.autocrlf", "false")
        self.assertIn(b"LICENSE.md", self.git(control, "status", "--porcelain"))
        self.assertEqual((self.sdk / "LICENSE.md").read_bytes(), b"SDK license\n")
        package.snapshot()

    def test_untracked_source(self):
        (self.repo / "new-source.cpp").write_bytes(b"untracked source\n")
        with self.assertRaisesRegex(package.PackageError, "untracked"):
            package.snapshot()

    def test_dirty_and_untracked_sdk(self):
        self.git(self.repo, "config", "submodule.4D-Plugin-SDK.ignore", "all")
        path = self.sdk / "LICENSE.md"
        original = path.read_bytes()
        path.write_bytes(b"modified SDK\n")
        with self.assertRaisesRegex(package.PackageError, "Source checkout is dirty"):
            package.snapshot()
        path.write_bytes(original)
        (self.sdk / "new-source.cpp").write_bytes(b"untracked SDK source\n")
        with self.assertRaisesRegex(package.PackageError, "Source checkout is dirty"):
            package.snapshot()

    def test_sdk_gitlink_mismatch(self):
        (self.sdk / "LICENSE.md").write_bytes(b"new SDK revision\n")
        self.git(self.sdk, "add", "LICENSE.md")
        self.git(self.sdk, "commit", "--quiet", "-m", "different SDK revision")
        with self.assertRaisesRegex(package.PackageError, "SDK checkout differs from source gitlink"):
            package.snapshot()

    def test_version_checkout_bytes_not_normalized(self):
        self.git(self.repo, "update-index", "--assume-unchanged", "VERSION")
        (self.repo / "VERSION").write_bytes(b"1.2.3\r\n")
        with self.assertRaisesRegex(package.PackageError, "VERSION bytes differ"):
            package.snapshot()

    def test_immutable_version_must_use_lf(self):
        self.git(self.repo, "config", "core.autocrlf", "false")
        (self.repo / ".gitattributes").write_bytes(b"VERSION -text\n")
        (self.repo / "VERSION").write_bytes(b"1.2.3\r\n")
        self.git(self.repo, "add", ".gitattributes", "VERSION")
        self.git(self.repo, "commit", "--quiet", "-m", "invalid CRLF VERSION")
        with self.assertRaisesRegex(package.PackageError, "VERSION must use LF"):
            package.snapshot()

    def test_dirty_snapshot_refuses_entrypoints_before_output(self):
        (self.repo / "new-source.cpp").write_bytes(b"untracked\n")
        destination = self.repo / "build-output"
        calls = (
            lambda: package.stage("windows", destination),
            lambda: package.assemble(self.repo / "missing-inputs", destination),
        )
        for call in calls:
            with self.assertRaisesRegex(package.PackageError, "Source checkout is dirty"):
                call()
            self.assertFalse(destination.exists())
        payload = self.repo / ".package-inputs/payload"
        payload.mkdir(parents=True)
        package.write_json(payload / "BUILD-PROVENANCE.json", {})
        with self.assertRaisesRegex(package.PackageError, "Source checkout is dirty"):
            package.inventory(payload, payload / "PROVENANCE.json")
        self.assertFalse((payload / "PROVENANCE.json").exists())


class NativeMetadataTests(unittest.TestCase):
    def test_readonly_fixture_cleanup(self):
        root = Path(__file__).resolve().parent / (".test-cleanup-" + uuid.uuid4().hex)
        root.mkdir()
        self.addCleanup(lambda: remove_fixture(root) if root.exists() else None)
        path = root / "readonly-object"
        path.write_bytes(b"fixture Git object")
        path.chmod(stat.S_IREAD)
        retry = Mock()
        error = PermissionError("fixture entry is read-only")
        remove_readonly(retry, str(path), (PermissionError, error, None))
        retry.assert_called_once_with(str(path))
        self.assertTrue(path.stat().st_mode & stat.S_IWUSR)
        with self.assertRaises(PermissionError):
            remove_readonly(retry, str(path), (PermissionError, error, None))
        other_error = OSError("not a read-only error")
        with self.assertRaises(OSError):
            remove_readonly(retry, str(path), (OSError, other_error, None))
        retry.reset_mock()
        with patch.object(Path, "is_symlink", return_value=True):
            with self.assertRaises(PermissionError):
                remove_readonly(retry, str(path), (PermissionError, error, None))
        retry.assert_not_called()
        path.chmod(stat.S_IREAD)
        remove_fixture(root)
        self.assertFalse(root.exists())

    def test_semver(self):
        for version in ("0.0.0", "1.2.3", "1.2.3-rc.1+build.42", "1.2.3-0"):
            self.assertIsNotNone(package.SEMVER.fullmatch(version), version)
        for version in ("01.2.3", "1.2", "v1.2.3", "1.2.3-01", "١.2.3"):
            self.assertIsNone(package.SEMVER.fullmatch(version), version)

    def test_cmake_compiler_parsing(self):
        root = Path(__file__).resolve().parent / (".test-compiler-" + uuid.uuid4().hex)
        root.mkdir()
        self.addCleanup(remove_fixture, root)
        compiler = root / "get-icon/build/CMakeFiles/4.0/CMakeCXXCompiler.cmake"
        compiler.parent.mkdir(parents=True)
        compiler.write_text('set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                            'set(CMAKE_CXX_COMPILER_VERSION "19.40")\n', encoding="utf-8")
        with patch.object(package, "ROOT", root), patch.object(
                package, "run", return_value=b"cmake version 4.0.0\n\n"):
            self.assertEqual(package.toolchain(), {
                "compiler_id": "MSVC", "compiler_version": "19.40", "cmake": "cmake version 4.0.0"})
            compiler.write_text('set(CMAKE_CXX_COMPILER_ID "MSVC")\n', encoding="utf-8")
            with self.assertRaisesRegex(package.PackageError, "VERSION"):
                package.toolchain()

    def test_native_architecture_and_dependency_contract(self):
        def output(args, cwd=None):
            if args[0] == "lipo":
                return b"x86_64 arm64\n"
            if args[1] == "-L":
                return b"binary:\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0)\n"
            return b"Load command 0\n"
        with patch.object(package.sys, "platform", "darwin"), patch.object(
                package, "run", side_effect=output):
            package.validate_macos(Path("binary"))
        for outputs in (
                [b"arm64\n"],
                [b"arm64 x86_64\n", b"binary:\n\t@rpath/evil.dylib (version 1)\n"],
                [b"arm64 x86_64\n", b"binary:\n\t/opt/homebrew/evil.dylib (version 1)\n"],
                [b"arm64 x86_64\n", output(["otool", "-L"]), b"cmd LC_RPATH\n"]):
            with self.subTest(outputs=outputs), patch.object(package.sys, "platform", "darwin"):
                with patch.object(package, "run", side_effect=outputs):
                    with self.assertRaises(package.PackageError):
                        package.validate_macos(Path("binary"))


if __name__ == "__main__":
    unittest.main()
