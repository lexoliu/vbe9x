#!/bin/sh
# Fetches the pinned Open Watcom v2 snapshot, verifies its SHA-256 and unpacks the host's
# tools with the 16- and 32-bit headers and libraries. Prints the WATCOM root on stdout.
# Cache: $VBE9X_CACHE, default ${XDG_CACHE_HOME:-~/.cache}/vbe9x.
set -eu

URL=https://github.com/open-watcom/open-watcom-v2/releases/download/2026-11-01-Build/ow-snapshot.tar.xz
SHA256=89e90b7544cf3ea4a38124ec4c104a5027f31101405d7115c3f833daa465bb48

case "$(uname -s)-$(uname -m)" in
Linux-x86_64) host=binl64 ;;
Darwin-arm64) host=armo64 ;;
*)
	echo "watcom.sh: no Open Watcom host tools for $(uname -s)-$(uname -m)" >&2
	exit 1
	;;
esac

cache=${VBE9X_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/vbe9x}
archive=$cache/ow-snapshot-$SHA256.tar.xz
root=$cache/watcom-$SHA256

sha256() {
	if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1
}

if [ ! -x "$root/$host/wcc" ]; then
	mkdir -p "$cache"
	if [ ! -f "$archive" ] || [ "$(sha256 "$archive")" != "$SHA256" ]; then
		curl -sSfL --retry 3 -o "$archive.part" "$URL"
		got=$(sha256 "$archive.part")
		if [ "$got" != "$SHA256" ]; then
			rm -f "$archive.part"
			echo "watcom.sh: $URL has SHA-256 $got, expected $SHA256" >&2
			exit 1
		fi
		mv "$archive.part" "$archive"
	fi
	rm -rf "$root.part"
	mkdir -p "$root.part"
	tar -xJf "$archive" -C "$root.part" "./$host" ./h ./lib286 ./lib386
	mv "$root.part" "$root"
fi

echo "$root"
