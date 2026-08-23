#!/bin/bash
# Build the EMU7800 IPK: the main PDK app, with the controller-support
# maintainer scripts (postinst/prerm) injected in.
#
# NOTE: this used to also bundle launcher/ (a Mojo stub app for Pre3 ROM
# shortcuts, id com.emu7800.touchpad.launcher) into the same ipk. That's
# disabled for now -- it showed up as a second, visually-identical "EMU7800"
# icon in the launcher grid (same title/icon as the main app) even on
# TouchPad, where it serves no purpose (TouchPad shortcuts point at the main
# app id directly, not the stub -- see device_is_small() in input.c), and
# its own actual launch path was never confirmed working on Pre3 either (see
# project-pre3-shortcut-status memory). Re-enable by restoring the
# palm-package/ar-merge steps from git history if that feature gets
# revisited with a distinct title/icon.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MAIN_VER=$(grep '"version"' appinfo.json | sed 's/.*"\([0-9.]*\)".*/\1/' | head -1)
OUTPUT="com.emu7800.touchpad_${MAIN_VER}_all.ipk"
WORK=$(mktemp -d)

echo "=== Building emu7800.arm ==="
# NOTE: the static launcher-stub fix for the OpenSSL-legacyWebOS Pre3
# LD_PRELOAD leak (plugin/src/launcher_stub.c, "make stub") is REVERTED from
# the release pipeline as of Aug 23, 2026 -- it broke launch on BOTH Pre3
# and TouchPad when tested on-device, root cause not yet diagnosed (the stub
# has no logging of its own, so we have zero visibility into why exec
# failed). Back to shipping the single dynamically-linked binary directly,
# the configuration that is confirmed working. See
# pre3-tls-patch-launcher-stub memory for status before re-attempting.
cd plugin && make && cd ..
cp plugin/emu7800.arm ./emu7800.arm
chmod 755 emu7800.arm

echo "=== Packaging main app (excluding launcher/ and plugin/) ==="
palm-package --exclude=launcher --exclude=plugin --exclude=*.sh --exclude=*.zip --exclude=*.ipk . -o "$WORK"

echo "=== Adding controller-support maintainer scripts (postinst/prerm) ==="
mkdir -p "$WORK/pkg" "$WORK/ctrl"
cd "$WORK/pkg"
ar x "$WORK/$OUTPUT"
tar xzf control.tar.gz -C "$WORK/ctrl"
cp "$SCRIPT_DIR/plugin/packaging/postinst" "$WORK/ctrl/postinst"
cp "$SCRIPT_DIR/plugin/packaging/prerm" "$WORK/ctrl/prerm"
chmod 755 "$WORK/ctrl/postinst" "$WORK/ctrl/prerm"
tar czf control.tar.gz -C "$WORK/ctrl" .
ar cr "$OUTPUT" debian-binary control.tar.gz data.tar.gz

cp "$WORK/pkg/$OUTPUT" "$SCRIPT_DIR/"
rm -rf "$WORK"

echo "=== Done: $OUTPUT ==="
