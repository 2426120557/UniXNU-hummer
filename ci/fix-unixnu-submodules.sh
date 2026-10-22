#!/usr/bin/env bash
#
# Repair the UniXNU checkout before `unixnu build ...` runs.
#
# `unixnu` runs `git submodule update --init --recursive` itself (scripts/unixnu/
# feeds.py, update_submodules) and treats a failure as fatal, so the build never
# starts if one gitlink is unusable.  UniXNU/UniXNU's `main` currently records
# three gitlinks whose commits the submodule remotes no longer serve (upstream
# force-pushed):
#
#   drivers/DarwinBoot      d099354d0eab34df5088ca1ba89c9c88bc3260bb
#   drivers/NativeAdreno    5cf11357a1b35ca8424baa2721e4578df1890bde
#   drivers/NativeRamBlock  d4d22d9819b0162eda49f6402235dddc2279410d
#
# Rather than hard-coding that list (it will rot), walk every submodule in
# .gitmodules, update it on its own, and when that fails re-point the gitlink at
# the remote's default-branch head -- which by definition is being served -- and
# try again.  The re-point is index-only: nothing is committed, and the engine
# still gets a fully populated tree.
#
# Usage:  cd UniXNU && bash ../ci/fix-unixnu-submodules.sh
#
set -uo pipefail

export GIT_TERMINAL_PROMPT=0

if [ ! -f .gitmodules ]; then
	echo "fix-unixnu-submodules: no .gitmodules here; run from the UniXNU checkout" >&2
	exit 1
fi

if ! git config -f .gitmodules --get-regexp '^submodule\..*\.path$' > submodules.txt; then
	echo "fix-unixnu-submodules: .gitmodules has no submodule entries"
	rm -f submodules.txt
	exit 0
fi

fixed=0
failed=0

# No pipe: the loop body has to be able to set the counters.
while read -r key path; do
	[ -n "${path:-}" ] || continue
	name=${key#submodule.}
	name=${name%.path}

	if git submodule update --init --recursive -- "$path" < /dev/null; then
		continue
	fi

	url=$(git config -f .gitmodules --get "submodule.$name.url")
	case "$url" in
		./*|../*) url="https://github.com/UniXNU/$(basename "$path").git" ;;
	esac

	sha=$(git ls-remote "$url" HEAD 2>/dev/null | awk '$2 == "HEAD" { print $1; exit }')
	if [ -z "$sha" ]; then
		echo "::error::$path is pinned to a commit upstream no longer serves, and $url advertises no HEAD"
		failed=$((failed + 1))
		continue
	fi

	old=$(git ls-tree HEAD -- "$path" | awk '{ print $3 }')
	echo "::warning::$path is pinned to ${old:-<none>}, which $url no longer serves; repointing to $sha"

	git update-index --cacheinfo "160000,$sha,$path" || {
		echo "::error::$path: could not rewrite the gitlink"
		failed=$((failed + 1))
		continue
	}
	rm -rf "$path"
	if git submodule update --init --recursive -- "$path" < /dev/null; then
		fixed=$((fixed + 1))
	else
		echo "::error::$path still fails after being repointed to $sha"
		failed=$((failed + 1))
	fi
done < submodules.txt

rm -f submodules.txt
echo "fix-unixnu-submodules: $fixed repointed, $failed failed"

[ "$failed" -eq 0 ]
