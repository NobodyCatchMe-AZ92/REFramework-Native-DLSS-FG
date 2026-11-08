#!/bin/sh
# Builds the release zip "Native DLSS + Frame Generation" in dist/ (local only, nothing is uploaded), laid out for Fluffy
# Mod Manager (and manual installs: the same folders go into the game folder), then checks it for personal information.
#   modinfo.ini, screenshot.jpg                         Fluffy Mod Manager's metadata (packaging/; version filled in)
#   reframework/plugins/refw_framegen.dll               the plugin
#   reframework/autorun/refw_framegen.lua               its menu
#   reframework/data/Native DLSS + Frame Generation/    NVIDIA's files (Streamline 2.12, DLSS / DLSS-G 310.8, signed by
#                                                       NVIDIA), their licenses, README, notices, VERSION.txt
# Everything inside reframework/: nothing goes next to the game's executable. NVIDIA's DLLs are kept out of git:
# third_party/nvidia/bin (or NVIDIA_BIN=...).
set -eu
P="$(cd "$(dirname "$0")" && pwd)"
STAGE="$P/dist/NativeDLSS_+_FG"
VERSION="${VERSION:-$(date +%Y%m%d)-$(git -C "$P" rev-parse --short HEAD)}"
NVIDIA_BIN="${NVIDIA_BIN:-$P/third_party/nvidia/bin}"
NVIDIA_FILES="sl.interposer.dll sl.common.dll sl.dlss.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlss.dll nvngx_dlssg.dll"
DOCS="$STAGE/reframework/data/Native DLSS + Frame Generation"

# The version shown in the menu and overlay (the Lua script) and logged by the plugin must be the package's.
if [ "${VERSION#*-}" = "$VERSION" ]; then # a release version (not a date-hash test build)
    grep -q "^local MOD_VERSION = \"$VERSION\"" "$P/lua/refw_framegen.lua" || { echo "lua/refw_framegen.lua MOD_VERSION isn't $VERSION"; exit 1; }
    grep -q "MOD_VERSION = \"$VERSION\";" "$P/src/framegen.cpp" || { echo "src/framegen.cpp MOD_VERSION isn't $VERSION"; exit 1; }
fi

rm -rf "$STAGE" && mkdir -p "$STAGE/reframework/plugins" "$STAGE/reframework/autorun" "$DOCS/licenses"
cp "$P/build/Release/refw_framegen.dll" "$STAGE/reframework/plugins/"
cp "$P/lua/refw_framegen.lua" "$STAGE/reframework/autorun/"

for f in $NVIDIA_FILES; do
    [ -f "$NVIDIA_BIN/$f" ] || { echo "missing NVIDIA file: $NVIDIA_BIN/$f"; exit 1; }
    cp "$NVIDIA_BIN/$f" "$DOCS/"
done

cp "$P/third_party/nvidia/DLSS_LICENSE.txt" "$DOCS/licenses/NVIDIA_RTX_SDKs_LICENSE.txt"
cp "$P/third_party/nvidia/Streamline_LICENSE.txt" "$DOCS/licenses/NVIDIA_Streamline_LICENSE.txt"
cp "$P/README.md" "$P/THIRD_PARTY_NOTICES.md" "$DOCS/"
echo "$VERSION" > "$DOCS/VERSION.txt"

# Fluffy Mod Manager's metadata: the template with this version.
sed -b "s/^version=[^\r]*/version=$VERSION/" "$P/packaging/modinfo.ini" > "$STAGE/modinfo.ini" # (-b: its CRLF line ends kept)
cp "$P/packaging/screenshot.jpg" "$STAGE/"

# Personal information: home paths and e-mail addresses (strings in the DLLs included). Add your own names with
# PII_PATTERN='name|other' ./package.sh.
PATTERN="users[\\/]+[a-z]|@gmail\\.com|@outlook\\.com|@proton\\.me${PII_PATTERN:+|$PII_PATTERN}"
if grep -a -i -E -l "$PATTERN" -r "$STAGE"; then
    echo "PERSONAL INFORMATION FOUND in the files above: not packaged"; exit 1
fi

ZIP="$P/dist/NativeDLSS_+_FG_$VERSION.zip"
rm -f "$ZIP"
# Windows' own tar (bsdtar) writes standard zip paths with "/"; PowerShell 5.1's Compress-Archive wrote "\", which some
# extractors and mod managers turn into file names.
(cd "$STAGE" && "$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" -a -c -f "$(cygpath -w "$ZIP")" modinfo.ini screenshot.jpg reframework)
echo "packaged $VERSION:"; (cd "$STAGE" && find . -type f | sort); echo "$(wc -c < "$ZIP") bytes: $ZIP"
