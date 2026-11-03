#!/bin/bash
set -euo pipefail

if [[ "${GITHUB_EVENT_NAME:-}" != workflow_dispatch || "${GITHUB_REF:-}" != refs/heads/main ]]; then
    echo "Production signing requires an explicit dispatch from main." >&2
    exit 1
fi
if [[ "$(uname -s)" != Darwin ]]; then
    echo "Developer ID signing requires macOS." >&2
    exit 1
fi

python3 - <<'PY'
import os
import re
names = [
    "APPLE_DEVELOPER_ID_CERTIFICATE", "APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD",
    "KEYCHAIN_PASSWORD", "NOTARYTOOL_APPLE_ID", "NOTARYTOOL_TEAM_ID", "NOTARYTOOL_PASSWORD",
]
missing = [name for name in names if not os.environ.get(name, "").strip()]
if missing:
    raise SystemExit("Required signing secrets are missing: " + ", ".join(missing))
if not re.fullmatch(r"[A-Z0-9]{10}", os.environ["NOTARYTOOL_TEAM_ID"]):
    raise SystemExit("NOTARYTOOL_TEAM_ID must be a 10-character Apple team ID")
PY

PAYLOAD="$1"
OUTPUT="$2"
BUNDLE="$PAYLOAD/get-icon.bundle"
VERSION=$(cat VERSION)
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
[[ -d "$BUNDLE" && ! -e "$OUTPUT" ]]
mkdir "$OUTPUT"
ORIGINAL_UMASK=$(umask)
umask 077
SIGNING_TEMP=$(mktemp -d "$RUNNER_TEMP/get-icon-signing.XXXXXX")
KEYCHAIN_PATH="$SIGNING_TEMP/signing.keychain-db"
CERT_PATH="$SIGNING_TEMP/certificate.p12"

cleanup() {
    status=$?
    if [[ -f "$SIGNING_TEMP/search-list" ]]; then
        if ! python3 scripts/release_checks.py keychain-search-list --snapshot "$SIGNING_TEMP/search-list"; then
            echo "Could not restore the original user keychain search list" >&2
            status=1
        fi
    fi
    if [[ -f "$KEYCHAIN_PATH" ]]; then
        if ! security delete-keychain "$KEYCHAIN_PATH"; then
            echo "Could not delete temporary signing keychain" >&2
            status=1
        fi
    fi
    rm -f "$CERT_PATH" "$SIGNING_TEMP/identities" "$SIGNING_TEMP/identity" \
        "$SIGNING_TEMP/bundle-signature" "$SIGNING_TEMP/dmg-signature" "$SIGNING_TEMP/search-list"
    if ! rmdir "$SIGNING_TEMP"; then
        echo "Could not remove temporary signing directory" >&2
        status=1
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '%s' "$APPLE_DEVELOPER_ID_CERTIFICATE" | base64 --decode -o "$CERT_PATH"
security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
security set-keychain-settings -lut 21600 "$KEYCHAIN_PATH"
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
python3 scripts/release_checks.py keychain-search-list \
    --snapshot "$SIGNING_TEMP/search-list" --add "$KEYCHAIN_PATH"
security import "$CERT_PATH" -P "$APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD" \
    -t cert -f pkcs12 -k "$KEYCHAIN_PATH" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
    -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH" > /dev/null
security find-identity -v -p codesigning "$KEYCHAIN_PATH" > "$SIGNING_TEMP/identities"
python3 - "$SIGNING_TEMP/identities" "$SIGNING_TEMP/identity" <<'PY'
import os
from pathlib import Path
import re
import sys
team = os.environ["NOTARYTOOL_TEAM_ID"]
identities = re.findall(r'([A-Fa-f0-9]{40}) "Developer ID Application: [^"\n]+ \(' +
                        re.escape(team) + r'\)"', Path(sys.argv[1]).read_text())
if len(identities) != 1:
    raise SystemExit("Expected exactly one valid Developer ID Application identity for the configured team")
Path(sys.argv[2]).write_text(identities[0])
PY
IDENTITY=$(cat "$SIGNING_TEMP/identity")
xcrun notarytool store-credentials get-icon-release --apple-id "$NOTARYTOOL_APPLE_ID" \
    --team-id "$NOTARYTOOL_TEAM_ID" --password "$NOTARYTOOL_PASSWORD" --keychain "$KEYCHAIN_PATH"
umask "$ORIGINAL_UMASK"

notarize() {
    local archive="$1"
    local format="$2"
    local report="$OUTPUT/$format-notarization.json"
    xcrun notarytool submit "$archive" --keychain-profile get-icon-release --keychain "$KEYCHAIN_PATH" \
        --wait --timeout 30m --output-format json > "$report"
    if ! python3 scripts/release_checks.py notarization --report "$report"; then
        local request_id
        request_id=$(python3 - "$report" <<'PY'
import json
from pathlib import Path
import sys
import uuid
print(uuid.UUID(json.loads(Path(sys.argv[1]).read_text())["id"]))
PY
        )
        xcrun notarytool log "$request_id" --keychain-profile get-icon-release \
            --keychain "$KEYCHAIN_PATH" > "$OUTPUT/$format-notarization-log.json"
        return 1
    fi
}

codesign --force --timestamp --options runtime --keychain "$KEYCHAIN_PATH" \
    --sign "$IDENTITY" "$BUNDLE"
codesign --verify --strict "$BUNDLE"
codesign --verify --strict "$BUNDLE/Contents/MacOS/get-icon"
codesign --display --verbose=4 "$BUNDLE" 2> "$SIGNING_TEMP/bundle-signature"
python3 scripts/release_checks.py signature --details "$SIGNING_TEMP/bundle-signature" \
    --team "$NOTARYTOOL_TEAM_ID" --runtime
python3 scripts/package.py inventory --payload "$PAYLOAD" --output "$PAYLOAD/PROVENANCE.json" \
    --team "$NOTARYTOOL_TEAM_ID"
python3 scripts/package.py verify --payload "$PAYLOAD" --manifest "$PAYLOAD/PROVENANCE.json"

ZIP="$OUTPUT/get-icon-$VERSION.zip"
DMG="$OUTPUT/get-icon-$VERSION.dmg"
ditto -c -k --sequesterRsrc "$PAYLOAD" "$ZIP"
notarize "$ZIP" zip

hdiutil create -format UDZO -volname get-icon -srcfolder "$PAYLOAD" "$DMG"
codesign --force --timestamp --keychain "$KEYCHAIN_PATH" --sign "$IDENTITY" "$DMG"
codesign --verify --strict "$DMG"
codesign --display --verbose=4 "$DMG" 2> "$SIGNING_TEMP/dmg-signature"
python3 scripts/release_checks.py signature --details "$SIGNING_TEMP/dmg-signature" \
    --team "$NOTARYTOOL_TEAM_ID"
notarize "$DMG" dmg
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
codesign --verify --strict "$DMG"
spctl --assess --type open --context context:primary-signature "$DMG"
cp "$PAYLOAD/PROVENANCE.json" "$OUTPUT/PROVENANCE.json"
python3 scripts/release_checks.py checksums --directory "$OUTPUT"
