#!/bin/bash
# Signs a release with your private key, which never leaves this machine. Run it right after GitHub Actions has
# published a release you tagged yourself. The server and the sticks install nothing without this signature.
#
# Before signing it checks that the files are what you think they are:
#  - the tag on GitHub points at the same commit as your local tag;
#  - a successful build of that commit published the files, uploaded by GitHub Actions;
#  - server.tar.gz holds exactly the files of that commit (the firmware cannot be rebuilt bit for bit, so it rests on
#    the two checks above).
# Then it writes release.txt (the version and the SHA-256 of each file) and signs it; a newer version is required on
# the other side, so an old signed release cannot be pushed again.
set -eu
cd "$(dirname "$0")"
TAG=${1:?usage: ./sign-release.sh vX.Y.Z}
KEY=${VARTOVYI_KEY:-$HOME/.config/vartovyi/release-key.pem}
REPO=${REPO:-Skerpsh/vartovyi}
SERVER_FILES="peace_bridge.py regions.json peace-stick-bridge.service install.sh add-caddy-routes.sh update.sh vartovyi-update.service vartovyi-update.timer webhook-public.pem"

[[ $TAG =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Tag must look like v1.4.0"; exit 1; }
commit=$(git rev-parse "$TAG^{commit}")
[ "$(gh api "repos/$REPO/commits/$TAG" --jq .sha)" = "$commit" ] || { echo "$TAG on GitHub is not your local $TAG"; exit 1; }
run=$(gh run list -R "$REPO" --workflow build.yml --branch "$TAG" --json headSha,conclusion --jq ".[] | select(.headSha == \"$commit\" and .conclusion == \"success\") | .headSha" | head -1)
[ "$run" = "$commit" ] || { echo "No successful build of $commit for $TAG"; exit 1; }
uploaders=$(gh api "repos/$REPO/releases/tags/$TAG" --jq '[.assets[] | select(.name == "server.tar.gz" or .name == "firmware.bin") | .uploader.login] | unique | join(",")')
[ "$uploaders" = "github-actions[bot]" ] || { echo "Release files were uploaded by: $uploaders"; exit 1; }

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
gh release download "$TAG" -R "$REPO" -p server.tar.gz -p firmware.bin -D "$dir"
mkdir "$dir/server"
tar -xzf "$dir/server.tar.gz" -C "$dir/server"
[ "$(ls "$dir/server" | sort | tr '\n' ' ')" = "$(printf '%s\n' $SERVER_FILES release-public.pem | sort | tr '\n' ' ')" ] ||
  { echo "server.tar.gz holds other files than expected"; exit 1; }
for f in $SERVER_FILES; do
  git show "$commit:vps/$f" | cmp -s - "$dir/server/$f" || { echo "server.tar.gz: $f differs from $TAG"; exit 1; }
done
git show "$commit:keys/release-public.pem" | cmp -s - "$dir/server/release-public.pem" || { echo "server.tar.gz: release key differs"; exit 1; }

(cd "$dir" && { echo "version $TAG"; sha256sum firmware.bin server.tar.gz; }) >"$dir/release.txt"
openssl dgst -sha256 -sign "$KEY" -out "$dir/release.der" "$dir/release.txt"
openssl dgst -sha256 -verify keys/release-public.pem -signature "$dir/release.der" "$dir/release.txt" >/dev/null  # the right key
base64 -w0 "$dir/release.der" >"$dir/release.txt.sig"
gh release upload "$TAG" -R "$REPO" --clobber "$dir/release.txt" "$dir/release.txt.sig"
echo "Signed $TAG ($commit). The server installs it within an hour."
