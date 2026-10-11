#!/usr/bin/env bash
# Builds a self-contained meshframe.app for macOS. Run from anywhere:
#
#   scripts/build-macos-app.sh [OUT_DIR]
#
# Writes OUT_DIR/meshframe.app (OUT_DIR defaults to ./build-app). The bundle
# holds the editor, meshframe-cli, and copies of every non-system library they
# load, so it runs without Homebrew. The bundle has an ad-hoc signature, so it
# runs on the Mac that built it. Other Macs need a Developer ID signature and
# notarization, or the user must remove the quarantine attribute.
#
# Needs the build dependencies from the README and rsvg-convert
# (brew install librsvg) to draw the icon.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mkdir -p "${1:-$root/build-app}" && cd "${1:-$root/build-app}" && pwd)"
build="$out/cmake"
app="$out/meshframe.app"
contents="$app/Contents"
frameworks="$contents/Frameworks"
version="$(sed -n 's/^project(meshframe VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMESHFRAME_TESTS=OFF
cmake --build "$build"

rm -rf "$app"
mkdir -p "$contents/MacOS" "$contents/Resources" "$frameworks"
cp "$build/meshframe" "$build/meshframe-cli" "$contents/MacOS/"

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>meshframe</string>
  <key>CFBundleDisplayName</key><string>meshframe</string>
  <key>CFBundleIdentifier</key><string>com.tripplyons.meshframe</string>
  <key>CFBundleExecutable</key><string>meshframe</string>
  <key>CFBundleIconFile</key><string>meshframe</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.video</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>meshframe project</string>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>LSHandlerRank</key><string>Owner</string>
      <key>LSItemContentTypes</key><array><string>com.tripplyons.meshframe.project</string></array>
    </dict>
  </array>
  <key>UTExportedTypeDeclarations</key>
  <array>
    <dict>
      <key>UTTypeIdentifier</key><string>com.tripplyons.meshframe.project</string>
      <key>UTTypeDescription</key><string>meshframe project</string>
      <key>UTTypeConformsTo</key><array><string>public.data</string></array>
      <key>UTTypeTagSpecification</key>
      <dict>
        <key>public.filename-extension</key><array><string>mfproj</string></array>
      </dict>
    </dict>
  </array>
</dict>
</plist>
EOF

# Icon: render the SVG at each size iconutil expects.
iconset="$out/meshframe.iconset"
rm -rf "$iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
    rsvg-convert -w "$size" -h "$size" "$root/assets/icon.svg" -o "$iconset/icon_${size}x${size}.png"
    rsvg-convert -w "$((size * 2))" -h "$((size * 2))" "$root/assets/icon.svg" -o "$iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$iconset" -o "$contents/Resources/meshframe.icns"
rm -rf "$iconset"

# Libraries: copy every non-system dependency into Frameworks and point each
# reference at the copy.

is_system() { [[ "$1" == /usr/lib/* || "$1" == /System/* ]]; }

# Prints the libraries FILE loads, without its own install name.
deps_of() {
    local self
    self="$(otool -D "$1" | sed -n 2p)"
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        [[ "$dep" == "$self" ]] || echo "$dep"
    done
}

# Prints the absolute path of REF as loaded by FILE, which lives at ORIGIN.
resolve() {
    local file="$1" origin="$2" ref="$3" rpath candidate
    case "$ref" in
        @loader_path/*) candidate="$(dirname "$origin")/${ref#@loader_path/}" ;;
        @executable_path/*) candidate="$build/${ref#@executable_path/}" ;;
        @rpath/*)
            while read -r rpath; do
                rpath="${rpath/@loader_path/$(dirname "$origin")}"
                if [[ -e "$rpath/${ref#@rpath/}" ]]; then
                    candidate="$rpath/${ref#@rpath/}"
                    break
                fi
            done < <(otool -l "$file" | awk '/cmd LC_RPATH/ {getline; getline; print $2}')
            : "${candidate:=$(brew --prefix)/lib/${ref#@rpath/}}"
            ;;
        *) candidate="$ref" ;;
    esac
    [[ -e "$candidate" ]] || { echo "cannot resolve $ref from $origin" >&2; exit 1; }
    realpath "$candidate"
}

# Each queue entry is "bundled file|original path".
queue=("$contents/MacOS/meshframe|$build/meshframe" "$contents/MacOS/meshframe-cli|$build/meshframe-cli")
while ((${#queue[@]})); do
    file="${queue[0]%%|*}"
    origin="${queue[0]#*|}"
    queue=("${queue[@]:1}")
    chmod u+w "$file"
    [[ "$file" == "$frameworks"/* ]] && install_name_tool -id "@rpath/$(basename "$file")" "$file" 2>/dev/null
    for ref in $(deps_of "$file"); do
        is_system "$ref" && continue
        name="$(basename "$ref")"
        if [[ ! -e "$frameworks/$name" ]]; then
            src="$(resolve "$file" "$origin" "$ref")"
            cp "$src" "$frameworks/$name"
            queue+=("$frameworks/$name|$src")
        fi
        install_name_tool -change "$ref" "@executable_path/../Frameworks/$name" "$file" 2>/dev/null
    done
done

if otool -L "$contents/MacOS/"* "$frameworks/"* | grep -E '^\s+/(opt|usr/local)/'; then
    echo "the bundle still references libraries outside itself" >&2
    exit 1
fi

# install_name_tool breaks existing signatures; sign the libraries, then the app.
codesign --force --sign - "$frameworks/"*
codesign --force --sign - "$contents/MacOS/meshframe-cli"
codesign --force --sign - "$app"

echo "$app"
