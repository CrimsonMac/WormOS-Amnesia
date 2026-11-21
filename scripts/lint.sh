#!/bin/bash
# Quick checks that need no build: recipes, shell and Python syntax, links
# between the documents, and files that must never be committed. Run by CI
# on every push; run it before committing too.
#
# Usage: scripts/lint.sh

set -uo pipefail
cd "$(dirname "$0")/.."

failures=0
fail() { printf 'FAIL  %s\n' "$*"; failures=$((failures + 1)); }

# ---- recipes ----
# Each recipe is read the way tools/wbuild reads it: its name matches its
# directory, it has a version, every source has a SHA-256, and everything it
# depends on or recommends has a recipe.
recipes=0
for dir in recipes/*/; do
	name=${dir#recipes/}
	name=${name%/}
	r=$dir/recipe
	[ -f "$r" ] || { fail "$name: no recipe file"; continue; }
	recipes=$((recipes + 1))
	bash -n "$r" 2>/dev/null || { fail "$name: recipe is not valid bash"; continue; }
	out=$(
		name="" version="" release=1 localsrc=""
		source=() sha256=() depends=() recommends=()
		# shellcheck disable=SC1090
		. "$r" > /dev/null 2>&1
		printf 'name=%s\nversion=%s\nrelease=%s\nlocalsrc=%s\n' "$name" "$version" "$release" "$localsrc"
		printf 'sources=%s\nsums=%s\n' "${#source[@]}" "${#sha256[@]}"
		for s in "${sha256[@]}"; do printf 'sum=%s\n' "$s"; done
		for d in "${depends[@]}" "${recommends[@]}"; do printf 'dep=%s\n' "${d%%:*}"; done
	)
	get() { sed -n "s/^$1=//p" <<< "$out"; }
	[ "$(get name | head -1)" = "$name" ] || fail "$name: recipe declares name '$(get name | head -1)'"
	[ -n "$(get version | head -1)" ] || fail "$name: no version"
	[[ $(get release | head -1) =~ ^[0-9]+$ ]] || fail "$name: release is not a number"
	[ "$(get sources)" = "$(get sums)" ] ||
		fail "$name: $(get sources) sources but $(get sums) checksums"
	while read -r s; do
		[ -z "$s" ] || [[ $s =~ ^[0-9a-f]{64}$ ]] || [ "$s" = SKIP ] ||
			fail "$name: malformed checksum '$s'"
	done <<< "$(get sum)"
	while read -r d; do
		# glibc and gcc-libs come from the toolchain (scripts/build-toolchain.sh).
		case $d in ''|glibc|gcc-libs) continue ;; esac
		[ -d "recipes/$d" ] || fail "$name: depends on '$d', which has no recipe"
	done <<< "$(get dep)"
done
printf 'recipes: %d checked\n' "$recipes"

# ---- shell scripts ----
shells=0
while IFS= read -r -d '' f; do
	first=$(head -c 64 "$f" | head -1)
	case $first in
	'#!/bin/bash'*|'#!/usr/bin/env bash'*) checker=(bash -n) ;;
	'#!/bin/sh'*) checker=(sh -n) ;;
	*) continue ;;
	esac
	shells=$((shells + 1))
	"${checker[@]}" "$f" 2>/dev/null || fail "$f: shell syntax error ($("${checker[@]}" "$f" 2>&1 | head -1))"
done < <(git ls-files -z -- scripts tools recipes tests kernel Makefile)
printf 'shell scripts: %d checked\n' "$shells"

# ---- Python ----
pys=0
while IFS= read -r -d '' f; do
	case $f in
	*.py) ;;
	*) head -1 "$f" | grep -q '^#!.*python' || continue ;;
	esac
	pys=$((pys + 1))
	python3 - "$f" <<-'EOF' || fail "$f: Python syntax error"
	import ast, sys
	ast.parse(open(sys.argv[1], encoding="utf-8").read(), sys.argv[1])
	EOF
done < <(git ls-files -z -- tests tools scripts recipes)
printf 'Python files: %d checked\n' "$pys"

# ---- links between documents ----
links=0
while IFS= read -r -d '' doc; do
	base=$(dirname "$doc")
	while read -r target; do
		target=${target%%#*}
		[ -n "$target" ] || continue
		links=$((links + 1))
		[ -e "$base/$target" ] || fail "$doc: link to missing file '$target'"
	done < <(grep -o '\](\([^)#:]*\)\(#[^)]*\)\?)' "$doc" | sed 's/^](\(.*\))$/\1/' |
		grep -v '^https\?://\|^mailto:')
done < <(git ls-files -z -- '*.md')
printf 'document links: %d checked\n' "$links"

# ---- files that must never be committed ----
# Symbolic links must stay inside the repository (a link to a folder on
# the build machine breaks every other checkout).
while IFS= read -r f; do
	target=$(git cat-file -p "HEAD:$f" 2>/dev/null || readlink "$f")
	case $target in
	/*|*../../*) fail "$f: symbolic link outside the repository ($target)" ;;
	esac
done < <(git ls-files -s | awk '$1 == "120000" { print $4 }')
# By content: private keys of any kind, and wsign's secret key (a 64-byte
# hex line, as ~/.config/wormos/core.key holds; public keys have 32 bytes).
while IFS= read -r f; do
	fail "$f: looks like a secret key"
done < <(git ls-files -z | xargs -0 grep -IlE -e '-----BEGIN [A-Z ]*PRIVATE KEY-----' \
	-e '^[0-9a-f]{128}$' -- 2>/dev/null)
while IFS= read -r -d '' f; do
	size=$(stat -c %s "$f" 2>/dev/null || echo 0)
	[ "$size" -le 5242880 ] || fail "$f: $((size / 1048576)) MB; large files belong in releases"
done < <(git ls-files -z)
while IFS= read -r f; do
	fail "$f: contains a path in the build machine's home"
done < <(git ls-files -z | xargs -0 grep -IlF -e "$HOME/" -- 2>/dev/null)

if [ "$failures" -gt 0 ]; then
	printf '%d problems\n' "$failures"
	exit 1
fi
echo "all checks passed"
