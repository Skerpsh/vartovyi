#!/bin/sh
# Run as root. Installs the latest GitHub release (bridge and stick firmware) when it is new; vartovyi-update.timer runs it hourly.
# Nothing is installed unless the release's manifest (release.txt: version and SHA-256 of each file) carries a valid
# signature by the release key already on this server (/etc/peace-stick/release-public.pem), the files match it, and
# the version is newer than the one installed: a release by someone else, or an old one, is ignored. See sign-release.sh.
set -eu
REPO=${REPO:-Skerpsh/vartovyi}
SEEN=/opt/peace-stick/release
KEY=/etc/peace-stick/release-public.pem
[ -s "$KEY" ] || { echo "No release key in $KEY: install once with deploy.sh" >&2; exit 1; }

tag=$(curl -fsS "https://api.github.com/repos/$REPO/releases/latest" | python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])')
[ "$tag" = "$(cat "$SEEN" 2>/dev/null)" ] && exit 0
echo "Installing $tag"

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
base=https://github.com/$REPO/releases/download/$tag
# The signed manifest comes after the build, from the owner's machine; until then, wait for the next run.
curl -fsSL -o "$dir/release.txt" "$base/release.txt" 2>/dev/null && curl -fsSL -o "$dir/release.txt.sig" "$base/release.txt.sig" 2>/dev/null ||
  { echo "$tag is not signed yet"; exit 0; }
base64 -d "$dir/release.txt.sig" >"$dir/release.der" && openssl dgst -sha256 -verify "$KEY" -signature "$dir/release.der" "$dir/release.txt" >/dev/null ||
  { echo "$tag: bad signature, not installed" >&2; exit 1; }
version=$(sed -n 's/^version //p' "$dir/release.txt")
[ "$version" = "$tag" ] || { echo "$tag: the manifest says $version, not installed" >&2; exit 1; }
installed=$(cat "$SEEN" 2>/dev/null || echo v0.0.0)
[ "$(printf '%s\n%s\n' "$installed" "$version" | sort -V | tail -1)" = "$version" ] ||
  { echo "$version is older than $installed, not installed"; exit 0; }
curl -fsSL -o "$dir/server.tar.gz" "$base/server.tar.gz"
curl -fsSL -o "$dir/firmware.bin" "$base/firmware.bin"
grep -E '^[0-9a-f]{64}  (server\.tar\.gz|firmware\.bin)$' "$dir/release.txt" >"$dir/sums"
[ "$(wc -l <"$dir/sums")" = 2 ] && (cd "$dir" && sha256sum -c --quiet sums) ||
  { echo "$tag: files do not match the signed manifest, not installed" >&2; exit 1; }
mkdir "$dir/server"
tar -xzf "$dir/server.tar.gz" -C "$dir/server"

sh "$dir/server/install.sh"
bash "$dir/server/add-caddy-routes.sh"

# The bin first, then the signed manifest the sticks check it with, the md5 (for the first firmware) last:
# a stick that sees something new finds the matching files.
out=/var/lib/peace-stick
for f in firmware.bin release.txt release.txt.sig; do
  install -m 644 "$dir/$f" "$out/$f.tmp" && mv "$out/$f.tmp" "$out/$f"
done
md5sum "$out/firmware.bin" | cut -d' ' -f1 >"$out/firmware.md5.tmp" && chmod 644 "$out/firmware.md5.tmp" && mv "$out/firmware.md5.tmp" "$out/firmware.md5"
echo "$tag" >"$SEEN"
echo "Installed $tag"
