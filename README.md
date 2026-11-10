# get-icon

English | [日本語](README.ja.md)

A native 4D plugin returning file and folder icons as **PNG-backed Pictures with alpha**, on macOS and Windows. Uses Finder/Windows Shell icon resolution, including OS-recognized custom file and folder icons. Does not generate thumbnails or parse custom-icon metadata itself.

## Requirements

- 4D 21.1 or later.
- macOS with AppKit, or Windows 10/11 (x64).
- The command is cooperative-only (`threadSafe: false`).

## Command

```4d
$picture:=Path Get icon($entry {; $size {; $options}})
```

Manifest signature: `Path Get icon(&J;&L;&L):P`.

| Parameter | Type | Meaning |
|---|---|---|
| `$entry` | Object | `4D.File` or `4D.Folder` referencing an existing filesystem entry; not a Text path |
| `$size` | Integer | Square output size in physical pixels; optional, defaults to **256** |
| `$options` | Integer | Optional overlay policy; see constants below. Omitted, **0**, or an unrecognized value uses the system-selected overlay |
| `$picture` | Picture | Encoded PNG on success; genuinely empty Picture on failure |

```4d
var $file; $folder : Object
var $picture : Picture
$file:=File("/LOGS/report.pdf")
$picture:=Path Get icon($file)
$folder:=Folder(fk database folder)
$picture:=Path Get icon($folder; 128)
// For a Windows .lnk file or macOS Finder alias:
// $picture:=Path Get icon($shortcut; 256; Icon No shortcut arrow)
// Omit all supported overlays, including non-shortcut Shell status badges:
// $picture:=Path Get icon($file; 256; Icon No overlays)
If (Picture size($picture)=0)
    // Missing path, invalid size, or native extraction/encoding failure
End if
```

4D materializes an omitted declared Longint as zero. Consequently **zero also selects the 256-pixel default**; negative values fail. The pinned SDK has no parameter-presence/count accessor; no private ABI inspection is used. Sizes whose four-channel buffer exceeds WIC's `UINT32_MAX` byte count fail before native allocation (maximum arithmetic-safe side: 32767 pixels, shared across platforms). This is a native representability bound, not a practical memory guarantee: large requests can still fail allocation. Only the encoded PNG length must fit the SDK's signed 32-bit length.

Output is exactly `$size` by `$size`, independent of screen DPI/Retina scaling. The whole source icon canvas, including intentional artwork margins, is fitted proportionally into the output without cropping. Smaller base-icon bitmaps are enlarged with nearest-neighbor sampling, preserving crisp pixel blocks rather than smoothing them: 32×32 to 128×128 gives 4×4 blocks; 32×32 to 256×256 gives 8×8 blocks. Downscaling retains high-quality filtering. Upscaling cannot invent detail or remove padding embedded in the artwork. Windows overlay badges are sized independently as described below.

**Windows:** the Shell-selected overlay is included by default for both files and folders, not just shortcuts. This can include shortcut arrows, version-control status, synchronization status, or sharing/access badges when Windows or an installed Shell extension exposes them through the classic overlay mechanism. Windows chooses one overlay; the plugin does not stack competing badges or invent priorities. Availability depends on the Windows version, installed handlers, configuration, and available system overlay slots. Modern cloud-status indicators are not necessarily exposed this way, and there is no generic badge for a file currently open/locked by another process.

Windows Explorer draws the UAC shield itself rather than through the overlay mechanism, so the plugin adds it separately to files when User Account Control is enabled: executables whose manifest requests `requireAdministrator` (or `highestAvailable` for an administrator account), executables with the compatibility setting **Run this program as an administrator**, and shortcuts marked **Run as administrator** or targeting such an executable. The shield is placed at the lower right, within half the output size, and is drawn beneath the shortcut arrow. Installer-name heuristics are not applied, so a shield shown by Explorer only because of a file name such as `setup.exe` is not reproduced.

`Icon No shortcut arrow` (**1**) suppresses only the system shortcut overlay, retaining an unrelated Shell-selected badge or UAC shield even on a shortcut. `Icon No overlays` (**2**) skips all separately composited Shell overlays and the UAC shield. Neither option removes decorations already embedded in the base icon artwork. These are mutually exclusive policies, not combinable bit flags; unrecognized values retain the default policy.

**macOS:** Finder aliases are resolved without UI or volume mounting. By default, aliases without their own custom icon use the target's native artwork with the system alias badge composited over it; this avoids the generic document artwork that `NSWorkspace` can return for a folder alias. An alias-specific custom icon is retained through `NSWorkspace` instead. Both suppression options use the target's icon. Unresolvable aliases fail explicitly. Finder Sync extension badges, cloud status, and lock decorations are not supported separately by the current backend.

The object type is used only to convert the path with 4D's matching constructor; it does not constrain the on-disk kind. Files, folders, Finder aliases, and Windows shortcuts all use the same command. A `4D.Folder` may reference a file or alias (for example, `Folder($alias.path+"/")`), and a `4D.File` may reference a directory using its separator-free native path. Trailing separators are removed before native lookup, preserving filesystem roots. On Windows, the actual filesystem attributes control file-only icon metadata and UAC shield handling, not the caller's object type. Shortcuts to files and folders are handled identically; an existing broken `.lnk` may still have a Shell-provided icon. No shortcut resolution UI, search, or link tracking runs.

Failures (including nonexistent/deleted paths, invalid objects, allocation or encoding errors) return `PA_CreatePicture(nullptr, 0)`, not a transparent PNG. The plugin logs a diagnostic to standard error. Returned pictures belong to 4D.

### Migration from 1.x

Version **2.0.0** replaces `File Get icon` and `Folder Get icon` with **Path Get icon** on both platforms. Replace both old command names in your 4D methods and recompile compiled projects. Arguments, default sizing, overlay constants, and PNG output remain unchanged. The old commands are removed, not retained under deprecated names. File/folder-kind mismatch rejection is intentionally removed.

## Constants

The **Icon options** theme exposes the following plugin constants through `constants.xlf`:

| Constant | Type | Value | Meaning |
|---|---|---|---|
| `Icon No shortcut arrow` | Longint | 1 | Suppress only the shortcut arrow; retain other Windows Shell overlays |
| `Icon No overlays` | Longint | 2 | Suppress all separately composited Windows Shell overlays and the UAC shield; suppress Finder alias arrows on macOS |

## Native implementation

macOS uses `NSWorkspace.iconForFile:` and a cleared 8-bit sRGB RGBA CoreGraphics bitmap at the requested dimensions, nearest-neighbor enlargement/high-quality reduction, and ImageIO PNG encoding. It does not mutate the cached `NSImage` or the current AppKit graphics context. One `PA_RunInMainProcess` callback covers native extraction, PNG encoding, SDK picture creation (including empty results), and error-path picture disposal. Parameter/property access stays in the caller's language context, and the completed picture is returned there without disposal. This boundary also applies to calls from listbox column data-source expressions. Although the SDK marks picture creation as thread-safe, it is kept in the same main-process cycle as extraction to avoid splitting the graphics lifecycle across contexts.

4D's main process and the Cocoa main thread are not interchangeable guarantees. The native backend separately verifies Cocoa main-thread execution before touching AppKit and reports an error if it is unavailable; it never synchronously dispatches onto the main queue. This is threading hardening, not proof that a reported intermittent crash has been resolved.

Alias-specific custom artwork is detected through the native Finder catalog flag; the deprecated catalog API is used because `NSURLCustomIconKey` is documented as unimplemented. For aliases without custom artwork, the target icon is rendered first, then the installed system `AliasBadgeIcon.icns` from `/System/Library/CoreServices/CoreTypes.bundle` is drawn over the complete output canvas with high-quality filtering. The badge supplies its own lower-left placement and transparent margins; the plugin does not approximate or redistribute Apple's icon assets. This resource location is a macOS implementation dependency, not a public API contract: a missing or unusable badge returns an explicit failure.

Windows resolves the actual UTF-16 path with `SHCreateItemFromParsingName` and requests `IShellItemImageFactory::GetImage` with `SIIGBF_ICONONLY`, leaving enlargement to WIC rather than Shell interpolation. `SHGetFileInfoW` with `SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX` supplies the Shell-selected overlay for any item. The overlay uses the smallest system image-list canvas that covers the requested size (small, large, extra-large, or jumbo), or the largest available canvas for bigger requests. Actual list dimensions are queried rather than assumed from screen DPI. Unlike the base icon, the overlay is never enlarged beyond that native canvas: a jumbo overlay normally retains its 256-pixel canvas on 512-pixel and larger outputs. Its complete transparent canvas is anchored at the lower left, without cropping the badge artwork; reductions use high-quality premultiplied-alpha filtering. This avoids magnifying the 32-pixel badge into a coarse, oversized arrow. The badge is alpha-composited after sizing the base icon. Shortcut-only suppression compares the selected overlay index with `SHGetIconOverlayIndexW`'s standard link overlay, rather than suppressing every badge on a link. Extraction/encoding uses native COM and WIC in memory, without temporary files, thumbnails, background-color flags, or GDI+ bitmap conversion.

On tested Windows builds, Shell extraction of legacy icons can return an oversized canvas rather than a correctly enlarged icon, even without `SIIGBF_SCALEUP`. Native Shell icon-location metadata, WIC ICO dimensions, and EXE/DLL group-icon resource dimensions allow extraction at the original canvas size before WIC scaling. Executable resources are loaded only as data, never executed; shortcut icon locations and targets are read without launching them. This never crops to the artwork's alpha bounds or removes intentional margins. The correction covers ICO files, ICO-backed Shell icons, embedded executable icons, and shortcuts using those sources, including Windows Installer advertised shortcuts whose icon files contain ICO data under an `.exe` name. When the source canvas is still unknown, the Shell can center a small icon on the requested canvas with a faint shadow frame; the plugin treats that as padding only when the centered region exactly matches the Shell's own rendering at a smaller standard size, and then enlarges that smaller rendering. Arbitrary third-party Shell providers may still supply low-resolution or padded artwork.

The UAC shield comes from `SHGetStockIconInfo(SIID_SHIELD)` in the same system image list selection as overlays. Elevation is determined from the `EnableLUA` policy, the executable's `RT_MANIFEST` `requestedExecutionLevel` (resources loaded as data only), the `AppCompatFlags\Layers` `RUNASADMIN` entry, and the shortcut's `SLDF_RUNAS_USER` flag or resolved target.

Filesystem objects are reconstructed using 4D's File/Folder constructor and `fk platform path`. The macOS backend receives the reconstructed POSIX `path`; Windows retains its Unicode `platformPath`. SDK property strings are copied before clearing their owning variables.

## Build and installation

Download the ZIP or DMG from [Releases](https://github.com/miyako/4d-plugin-get-icon/releases). Extract or mount it, then copy **get-icon.bundle** into your database's **Plugins** folder and restart 4D. Keep the bundle's `Contents` hierarchy intact. Both downloads include macOS universal and Windows x64 binaries; macOS is Developer ID signed/notarized, while Windows is not Authenticode signed.

The project adapts the tutorial's `example/` CMake and test-project scaffold. CMake is the single supported build surface; generate IDE projects with `-G Xcode` or the installed Visual Studio generator rather than maintaining stale template IDE projects.

```sh
git clone --recurse-submodules https://github.com/miyako/4d-plugin-get-icon.git
cd 4d-plugin-get-icon
cmake -S get-icon -B get-icon/build -DCMAKE_BUILD_TYPE=Release
cmake --build get-icon/build --config Release --parallel
ctest --test-dir get-icon/build -C Release --output-on-failure
```

Use `-A x64` when configuring on Windows. For a universal macOS bundle add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. Requires CMake 3.20+, Xcode on macOS or Visual Studio with the Windows SDK on Windows. No third-party image library is needed; the 4D SDK is pinned as a submodule.

Output is placed in `get-icon/get-icon-test/Plugins/`: `get-icon.bundle` on macOS or `get-icon/Contents/Windows64/get-icon.4DX` with sibling Resources on Windows. Copy that plugin directory to your database's **Plugins** folder and restart 4D. Local builds are not Developer ID signed/notarized.

The manifest uses a project-local ID (`31071`), not a claim of a registered production allocation. Ensure no other installed plugin uses this identity or these command names.

## Tests and CI

Native tests exercise pixel dimensions, PNG alpha, Unicode paths, custom icons, legacy source artwork, overlay policies, invalid inputs, and repeated extraction. Windows regressions compare overlay policies with live Shell selection for ordinary files, folders, and shortcuts, and check exact 4×4/8×8 source-pixel replication and lower-left shortcut compositing for links to files and folders. Large-badge regressions compare composed pixels with the native jumbo overlay at 128, 256, 257, 512, and 1024 pixels, using both legacy 32-pixel and native 256-pixel base icons; they verify that larger outputs do not enlarge the badge. Further regressions cover Windows Installer style ICO data named `.exe`, and UAC shields for `requireAdministrator` manifests, compatibility-layer elevation, and run-as-administrator shortcuts (restricted to the lower-right quadrant and removed by `Icon No overlays`). Third-party status overlays require their provider to be installed; tests do not install handlers or change system associations. The tutorial-derived 4D test project exercises the unified command with both object types, both named overlay constants, unrecognized-option defaults, default sizing, decoded PNG dimensions, repeated calls, and `Picture size(...)=0` on failures.

macOS regressions pass Finder aliases to files and folders with and without trailing separators, covering the default arrow and both suppression policies. They compare pixels outside the badge with the target's actual artwork, including ordinary folders, target custom icons, and alias-specific custom icons, rather than treating a generic document icon with an arrow as success. Windows regressions pass folder shortcuts to **Path Get icon** with and without a trailing separator and compare every overlay policy with the `.lnk` file result; shortcuts to files, broken shortcuts, plain files, and drive roots are covered. The 4D integration suite exercises this path on both platforms (extensionless aliases on macOS, `.lnk` shortcuts on Windows).

Run the integration suite with a matching [tool4d](https://developer.4d.com/docs/Admin/cli/) runtime (no license required):

```sh
/path/to/tool4d --dataless --startup-method=test_all \
  --project="$PWD/get-icon/get-icon-test/Project/get-icon.4DProject"
```

Require both a successful exit and an exact `PASS` output line: tool4d can exit zero after a language error. The integration suite also interleaves 256-pixel and 31-pixel plugin icons with native `.getIcon()` calls for 100 iterations in another cooperative 4D process. A macOS SDK-callback harness verifies that rendering, picture creation, and failure cleanup stay inside the main-process callback, including dispatch failures, while returning a picture transfers ownership in the caller's context. These checks do not reproduce interactive listbox data-source evaluation. `.github/workflows/test.yml` builds and runs native/4D tests on macOS and Windows for pushes, pull requests, and manual dispatch.

**Normal builds and PR tests require no custom secrets.** Checkout uses GitHub's automatic `GITHUB_TOKEN` with read-only contents access; tool4d downloads publicly and needs no license activation. CI also stages the tested native binaries, checks their source-bound manifests, and validates the real same-run macOS/Windows artifact handoff. A test-only ad-hoc seal exercises strict bundle verification, ZIP/DMG round-trips and the combined plugin's smoke test; it is never uploaded as a distribution or accepted by production signing. The short-lived build inputs are not signed distribution packages.

## Signed distribution packages

`.github/workflows/release.yml` adds **macOS Developer ID Application signing and Apple notarization**. Windows Authenticode signing is intentionally not implemented: the included Windows `.4DX` remains unsigned.

Before each new release, run **Bump version** (`.github/workflows/bump-version.yml`) manually in GitHub Actions from **main**, choosing **patch** (default), **minor**, or **major**. It validates `VERSION`, rejects an already tagged target version, and commits the new version directly to **main** using the automatic `GITHUB_TOKEN`; no custom secrets are required. Repository rules must permit this workflow to push to main. It does not create a tag or start a release. Alternatively, update `VERSION` to an unused semantic version in a pull request and merge it into **main**.

Then run **Build and Release** manually in GitHub Actions, choosing **main**. Re-running a failed workflow uses its original commit and will not pick up a later version bump; start a new dispatch instead. Other refs are rejected by the production job and signing script. The dispatch's immutable commit is used for both platforms, the existing native/tool4d tests, input manifests, and packaging. `VERSION` remains the single version source; the release workflow never bumps it or merges code.

Only after all signing, notarization, verification and packaged smoke tests succeed, a separate publication job receives `contents: write`. It creates `v<VERSION>` at the exact dispatch commit, uploads a draft GitHub Release, downloads the uploaded assets and verifies their checksums, then publishes it. Existing tags are never moved; a tag for another commit or an existing release stops publication rather than overwriting assets. A failed draft upload remains a draft for inspection, not a public release. The other jobs and all normal CI remain read-only.

The workflow builds a universal macOS binary and Windows x64 binary, verifies their hashes, versions and source/SDK commits, and assembles one portable plugin:

```text
get-icon.bundle/
  Contents/
    Info.plist
    MacOS/get-icon
    Windows64/get-icon.4DX
    Resources/manifest.json
    Resources/constants.xlf
```

The packaging and verification utilities use Python 3.11+ and its standard library; the hosted runners provide this, with no additional Python packages required.

The bundle is Developer ID signed with hardened runtime and a secure timestamp. Signing identity and the resulting signature must match `NOTARYTOOL_TEAM_ID`. Verification, missing credentials, a signing error, or any notarization status other than **Accepted** fails the workflow; there is no ad-hoc or unsigned production fallback. The DMG container is separately signed and notarized, then stapled, validated, and assessed by Gatekeeper. The ZIP is notarized but cannot itself carry a stapled ticket; use the DMG for a stapled download.

After both formats pass, the ZIP is extracted, its final inventory and signature are verified, and the actual packaged plugin runs through tool4d before artifacts upload. Download `get-icon-signed-packages-<commit>` from the successful run. It contains the ZIP, DMG, notarization reports, `PROVENANCE.json`, and `SHA256SUMS`. The package includes plugin/SDK licenses and `BUILD-PROVENANCE.json` with explicitly pre-sign native hashes; `PROVENANCE.json` inventories the signed payload, while `SHA256SUMS` hashes the final archives **after signing and DMG stapling**:

```sh
shasum -a 256 -c SHA256SUMS
xcrun stapler validate get-icon-1.0.0.dmg
spctl --assess --type open --context context:primary-signature get-icon-1.0.0.dmg
```

### Required signing secrets

Use the same six names as the supplied migration-plugin workflow. Configure them under **Settings > Secrets and variables > Actions** (repository secrets), or as secrets of the **release-signing** environment:

| Secret | Purpose and setup |
|---|---|
| `APPLE_DEVELOPER_ID_CERTIFICATE` | Export a **Developer ID Application** certificate together with its private key from macOS Keychain Access as `.p12`; encode with `base64 -i certificate.p12 \| pbcopy` and paste the encoded value. An Apple Development certificate is not sufficient. |
| `APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD` | Password chosen when exporting that `.p12`. |
| `KEYCHAIN_PASSWORD` | A new strong random password used only for the temporary CI keychain. |
| `NOTARYTOOL_APPLE_ID` | Apple ID email authorized for the Apple Developer team. |
| `NOTARYTOOL_TEAM_ID` | The team's 10-character ID, available in the Apple Developer account's Membership details. It must match the signing certificate's team. |
| `NOTARYTOOL_PASSWORD` | An app-specific password created at [account.apple.com](https://account.apple.com/) under **Sign-In and Security > App-Specific Passwords**. |

No PAT, GitHub App credentials, Windows certificate, or additional 4D license is needed. The workflow uses a temporary keychain, grants only the signing tool access, and removes it on exit. Configure **Settings > Environments > release-signing** with main-only deployment restrictions and required reviewers if approval is desired; this does not require placing secrets in normal PR jobs.

Production Developer ID signing, notarization and Gatekeeper acceptance must be validated by an authorized dispatch after these credentials are configured. Secretless CI validates native builds, the artifact handoff, package layout/provenance, and fail-closed signing checks; it does not claim that a real signed/notarized distribution was produced.

## References and license

Native code and scaffold are under [MIT](LICENSE); see the SDK's separate license in its submodule.
