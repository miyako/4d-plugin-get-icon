# get-icon

A native 4D plugin returning file and folder icons as **PNG-backed Pictures with alpha**, on macOS and Windows. Uses Finder/Windows Shell icon resolution, including OS-recognized custom file and folder icons. Does not generate thumbnails or parse custom-icon metadata itself.

## Requirements

- 4D 21.1 or later.
- macOS with AppKit, or Windows 10/11 (x64).
- Commands are cooperative-only (`threadSafe: false`).

## Commands

```4d
$picture:=File Get icon($file {; $size})
$picture:=Folder Get icon($folder {; $size})
```

Manifest signatures: `File Get icon(&J;&L):P` and `Folder Get icon(&J;&L):P`.

| Parameter | Type | Meaning |
|---|---|---|
| `$file` / `$folder` | Object | Existing `4D.File` / `4D.Folder` |
| `$size` | Integer | Square output size in physical pixels; optional, defaults to **256** |
| `$picture` | Picture | Encoded PNG on success; genuinely empty Picture on failure |

```4d
var $file; $folder : Object
var $picture : Picture
$file:=File("/LOGS/report.pdf")
$picture:=File Get icon($file)
$folder:=Folder(fk database folder)
$picture:=Folder Get icon($folder; 128)
If (Picture size($picture)=0)
    // Missing path, invalid size, or native extraction/encoding failure
End if
```

4D materializes an omitted declared Longint as zero. Consequently **zero also selects the 256-pixel default**; negative values fail. The pinned SDK has no parameter-presence/count accessor; no private ABI inspection is used. Sizes whose four-channel buffer exceeds WIC's `UINT32_MAX` byte count fail before native allocation (maximum arithmetic-safe side: 32767 pixels, shared across platforms). This is a native representability bound, not a practical memory guarantee: large requests can still fail allocation. Only the encoded PNG length must fit the SDK's signed 32-bit length.

Output is exactly `$size` by `$size`, independent of screen DPI/Retina scaling. The whole source icon canvas, including intentional artwork margins, is fitted into the output without cropping. Upscaling a legacy 32-pixel icon cannot invent detail or remove padding embedded in its artwork.

Failures (including nonexistent/deleted paths, wrong file/folder kind, allocation or encoding errors) return `PA_CreatePicture(nullptr, 0)`, not a transparent PNG. The plugin logs a diagnostic to standard error. Returned pictures belong to 4D.

## Native implementation

macOS uses `NSWorkspace.iconForFile:` and a cleared 8-bit sRGB RGBA CoreGraphics bitmap at the requested dimensions, high-quality interpolation, and ImageIO PNG encoding. It does not mutate the cached `NSImage` or the current AppKit graphics context. The plugin uses `PA_RunInMainProcess` and verifies Cocoa main-thread execution before touching AppKit; it never synchronously dispatches onto the main queue.

Windows resolves the actual UTF-16 path with `SHCreateItemFromParsingName` and requests `IShellItemImageFactory::GetImage` with `SIIGBF_ICONONLY | SIIGBF_SCALEUP`. Extraction/encoding uses native COM and WIC in memory, without temporary files, thumbnails, background-color flags, or GDI+ bitmap conversion.

On tested Windows builds, Shell upscaling of legacy ICO resources can return an oversized canvas rather than a correctly enlarged icon. For ICO sources, native Shell icon-location metadata and WIC resource dimensions allow extraction at the original canvas size before WIC scaling. This never crops to the artwork's alpha bounds or removes intentional margins. The correction covers ICO files and ICO-backed Shell icons; arbitrary third-party Shell providers may still supply low-resolution or padded artwork.

Filesystem objects are reconstructed using 4D's File/Folder constructor and `fk platform path`. The macOS backend receives the reconstructed POSIX `path`; Windows retains its Unicode `platformPath`. SDK property strings are copied before clearing their owning variables.

## Build and installation

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

Native tests exercise pixel dimensions, PNG alpha, Unicode paths, custom icons, legacy source artwork, invalid inputs, and repeated extraction. The tutorial-derived 4D test project exercises both commands, default sizing, decoded PNG dimensions, repeated calls, and `Picture size(...)=0` on failures.

Run the integration suite with a matching [tool4d](https://developer.4d.com/docs/Admin/cli/) runtime (no license required):

```sh
/path/to/tool4d --dataless --startup-method=test_all \
  --project="$PWD/get-icon/get-icon-test/Project/get-icon.4DProject"
```

Require both a successful exit and an exact `PASS` output line: tool4d can exit zero after a language error. The integration suite also checks extraction from another cooperative 4D process. `.github/workflows/test.yml` builds and runs native/4D tests on macOS and Windows for pushes, pull requests, and manual dispatch.

**No repository secrets are required.** Checkout uses GitHub's automatic `GITHUB_TOKEN` with read-only contents access; tool4d downloads publicly and needs no license activation. No PAT, GitHub App, signing certificate, or notarization credentials are needed. There is no release/publishing workflow.

## References and license

Native code and scaffold are under [MIT](LICENSE); see the SDK's separate license in its submodule.
