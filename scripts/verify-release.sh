#!/bin/sh
# Check downloaded release files: SHA256SUMS against its signature (made
# with the WormOS key, keys/core.pem), then every listed file that is
# present against SHA256SUMS. Needs only OpenSSL 3 and coreutils, so it
# runs on any Linux:
#
#     curl -LO https://raw.githubusercontent.com/CrimsonMac/WormOS/main/keys/core.pem
#     sh verify-release.sh DIR [core.pem]
#
# Usage: scripts/verify-release.sh DIR [PUBLIC-KEY.pem]
set -eu
dir=${1:?usage: verify-release.sh DIR [PUBLIC-KEY.pem]}
key=${2:-$(dirname "$0")/../keys/core.pem}
[ -f "$key" ] || { echo "no public key at $key" >&2; exit 2; }
[ -f "$dir/SHA256SUMS" ] && [ -f "$dir/SHA256SUMS.sig" ] ||
	{ echo "$dir needs SHA256SUMS and SHA256SUMS.sig" >&2; exit 2; }
sig=$(mktemp)
trap 'rm -f "$sig"' EXIT
# The signature file is one line of hex (64 bytes): OpenSSL wants it raw.
tr -d ' \n' < "$dir/SHA256SUMS.sig" | sed 's/../\\x&/g' | xargs -0 printf '%b' > "$sig"
if ! openssl pkeyutl -verify -pubin -inkey "$key" -rawin -in "$dir/SHA256SUMS" -sigfile "$sig" \
	> /dev/null 2>&1; then
	echo "BAD SIGNATURE: SHA256SUMS was not signed with this key" >&2
	exit 1
fi
echo "SHA256SUMS: good signature"
(cd "$dir" && sha256sum -c --ignore-missing SHA256SUMS)
