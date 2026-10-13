#!/usr/bin/env bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Register the template provider with a DisplayXR runtime on a POSIX dev box
# (Linux, macOS): write NNN-<id>-conversion-provider.json into a plug-in root
# (conversion-provider-discovery.md section 3.5). The "-conversion-provider.json"
# suffix routes it to the conversion loader; the display and input loaders skip it.
#
#   scripts/register-dev.sh [register] [path/to/DisplayXR-ExampleConversion.so|.dylib]
#                           [--dir DIR] [--probe-order N]
#   scripts/register-dev.sh unregister [--dir DIR]
#
# DIR defaults to the first entry of XRT_PLUGIN_SEARCH_PATH, else the per-user
# root: ~/Library/Application Support/DisplayXR/DisplayProcessors (macOS) or
# ${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors (Linux).
#
# VENDOR TODO: change ID / NAME / VENDOR / the library name to yours.

set -euo pipefail

ID="example-conversion"
NAME="Example Conversion Provider (template)"
VENDOR="Example"
LIBBASE="DisplayXR-ExampleConversion"
ORDER=50

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

usage() {
	sed -n '10,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

mode="register"
if [[ "${1:-}" == "register" || "${1:-}" == "unregister" ]]; then
	mode="$1"
	shift
fi

lib=""
dir=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--dir) dir="${2:?--dir needs a directory}"; shift 2 ;;
	--probe-order) ORDER="${2:?--probe-order needs a number}"; shift 2 ;;
	-h | --help) usage; exit 0 ;;
	-*) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
	*) lib="$1"; shift ;;
	esac
done
if ! [[ "$ORDER" =~ ^[0-9]+$ ]]; then
	echo "ERROR: --probe-order must be a non-negative integer" >&2
	exit 1
fi

if [[ -z "$dir" ]]; then
	if [[ -n "${XRT_PLUGIN_SEARCH_PATH:-}" ]]; then
		dir="${XRT_PLUGIN_SEARCH_PATH%%:*}"
	elif [[ "$(uname -s)" == "Darwin" ]]; then
		dir="$HOME/Library/Application Support/DisplayXR/DisplayProcessors"
	else
		dir="${XDG_DATA_HOME:-$HOME/.local/share}/DisplayXR/DisplayProcessors"
	fi
fi

# One manifest per id: drop any earlier one (another ProbeOrder prefix).
remove_existing() {
	if [[ -d "$dir" ]]; then
		find "$dir" -maxdepth 1 -name "*-${ID}-conversion-provider.json" -type f -exec rm -f {} +
	fi
}

if [[ "$mode" == "unregister" ]]; then
	remove_existing
	echo "Removed conversion provider '$ID' manifests from: $dir"
	exit 0
fi

if [[ -z "$lib" ]]; then
	for cand in "$repo/build/$LIBBASE.so" "$repo/build/$LIBBASE.dylib" \
		"$repo/build/Release/$LIBBASE.so" "$repo/build/Release/$LIBBASE.dylib"; do
		if [[ -f "$cand" ]]; then
			lib="$cand"
			break
		fi
	done
fi
if [[ -z "$lib" || ! -f "$lib" ]]; then
	echo "ERROR: provider library not found${lib:+: $lib}. Build first: cmake -S . -B build && cmake --build build" >&2
	exit 1
fi
lib="$(cd "$(dirname "$lib")" && pwd)/$(basename "$lib")"

mkdir -p "$dir"
remove_existing
manifest="$dir/$(printf '%03d' "$ORDER")-${ID}-conversion-provider.json"
tmp="$(mktemp "$dir/.${ID}.XXXXXX")"
# JSON-escape the two free-text paths (backslash and double quote).
esc() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
cat >"$tmp" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "$ID",
        "display_name": "$(esc "$NAME")",
        "vendor":       "$(esc "$VENDOR")",
        "version":      "dev",
        "binary_path":  "$(esc "$lib")",
        "probe_order":  $ORDER
    }
}
EOF
# Atomic: the runtime never reads a half-written manifest.
mv -f "$tmp" "$manifest"
echo "Registered conversion provider '$ID' (ProbeOrder $ORDER):"
echo "  $manifest"
echo "  -> $lib"
echo "Settings: add a \"settings\" object to the manifest (see examples/manifest/)."
