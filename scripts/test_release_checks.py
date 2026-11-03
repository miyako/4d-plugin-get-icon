import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from release_checks import checksums, keychain_search_list, notarization, signature


TEAM = "AB12345678"
DETAILS = (
    "CodeDirectory v=20500 size=123 flags=0x10000(runtime) hashes=10+3 location=embedded\n"
    "Authority=Developer ID Application: Example (AB12345678)\n"
    "Authority=Developer ID Certification Authority\n"
    "Authority=Apple Root CA\n"
    "Timestamp=Nov 3, 2026 at 8:00:00 PM\n"
    "TeamIdentifier=AB12345678\n"
)


class ReleaseChecks(unittest.TestCase):
    def test_keychain_search_list_round_trip_preserves_quoted_paths(self):
        original = '    "/Users/runner/Library/Keychains/login.keychain-db"\n    "/tmp/with spaces/shared.keychain-db"\n'
        with tempfile.TemporaryDirectory() as temp:
            snapshot = Path(temp) / "search-list"
            signing = Path(temp) / "signing.keychain-db"
            with mock.patch("release_checks.subprocess.check_output", return_value=original), \
                    mock.patch("release_checks.subprocess.run") as run:
                keychain_search_list(snapshot, signing)
                self.assertEqual(snapshot.read_text(), original)
                keychain_search_list(snapshot)
            existing = ["/Users/runner/Library/Keychains/login.keychain-db",
                        "/tmp/with spaces/shared.keychain-db"]
            prefix = ["security", "list-keychains", "-d", "user", "-s"]
            self.assertEqual(run.call_args_list, [
                mock.call([*prefix, str(signing), *existing], check=True),
                mock.call([*prefix, *existing], check=True),
            ])

    def test_keychain_search_list_failure_keeps_restorable_snapshot(self):
        with tempfile.TemporaryDirectory() as temp:
            snapshot = Path(temp) / "search-list"
            with mock.patch("release_checks.subprocess.check_output", return_value='"/tmp/login"\n'), \
                    mock.patch("release_checks.subprocess.run",
                               side_effect=subprocess.CalledProcessError(1, "security")):
                with self.assertRaises(subprocess.CalledProcessError):
                    keychain_search_list(snapshot, Path(temp) / "signing")
            self.assertEqual(snapshot.read_text(), '"/tmp/login"\n')

    def test_developer_id_with_runtime(self):
        signature(DETAILS, TEAM, runtime=True)

    def test_reject_signature_failures(self):
        for invalid in (
            DETAILS.replace(TEAM, "CD12345678"),
            DETAILS.replace("Timestamp=", "Signed Time="),
            DETAILS.replace("Developer ID Application: ", "Apple Development: "),
            DETAILS.replace("flags=0x10000(runtime)", "flags=0x0(none)"),
            DETAILS + "Signature=adhoc\n",
        ):
            with self.subTest(details=invalid), self.assertRaises(ValueError):
                signature(invalid, TEAM, runtime=True)

    def test_dmg_does_not_require_runtime(self):
        signature(DETAILS.replace("flags=0x10000(runtime)", "flags=0x0(none)"), TEAM)

    def test_notary_must_be_explicitly_accepted(self):
        notarization({"id": "request-id", "status": "Accepted"})
        for report in ({}, {"status": "Accepted"}, {"id": "id", "status": "Invalid"},
                       {"id": "id", "status": "In Progress"}):
            with self.subTest(report=report), self.assertRaises(ValueError):
                notarization(report)

    def test_checksums_use_final_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / "get-icon.dmg"
            path.write_bytes(b"before signing")
            checksums(root)
            old = (root / "SHA256SUMS").read_bytes()
            path.write_bytes(b"after signing and stapling")
            checksums(root)
            expected = hashlib.sha256(path.read_bytes()).hexdigest() + "  get-icon.dmg\n"
            self.assertEqual((root / "SHA256SUMS").read_text(), expected)
            self.assertNotEqual((root / "SHA256SUMS").read_bytes(), old)

    @unittest.skipUnless(sys.platform == "darwin", "Production signing shell runs only on macOS")
    def test_signing_requires_main_dispatch_and_all_secrets(self):
        names = (
            "APPLE_DEVELOPER_ID_CERTIFICATE", "APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD",
            "KEYCHAIN_PASSWORD", "NOTARYTOOL_APPLE_ID", "NOTARYTOOL_TEAM_ID", "NOTARYTOOL_PASSWORD",
        )
        env = {key: value for key, value in os.environ.items() if key not in names}
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "distribution"
            for event, ref, expected in (
                ("pull_request", "refs/pull/1/merge", "explicit dispatch from main"),
                ("workflow_dispatch", "refs/heads/feature", "explicit dispatch from main"),
                ("workflow_dispatch", "refs/heads/main", "Required signing secrets are missing"),
            ):
                env.update(GITHUB_EVENT_NAME=event, GITHUB_REF=ref)
                result = subprocess.run(
                    ["bash", "scripts/sign-package.sh", temp, str(output)],
                    env=env, text=True, capture_output=True, check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
