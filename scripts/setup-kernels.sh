#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Prepare the kernel trees the poc-version driver is built against.
#
# This is the entry point on any Linux host; the devcontainer only installs
# the packages and runs it once when it is created. It is idempotent: work
# that is already done is skipped, so running it again only does what is
# missing.
#
# Usage: setup-kernels.sh [KERNEL_ROOT]
#
# KERNEL_ROOT defaults to $KERNELS; one of them must be given. Pass the same
# directory to make as KERNELS. Layout it creates:
#
#   src/v5.15/              shallow clone of the minimum kernel (ADR-0002)
#   src/v6.4/               shallow clone of the first kernel with the maple
#                           cache, so both sides of each version guard build
#   src/<tag>/              shallow clone of the latest stable tag
#   src/latest -> <tag>     stable name the Makefile uses
#   build/<tree>-<arch>/    O= build dir: defconfig + modules_prepare
#
# where <tree> is "v5.15", "v6.4" or "latest" and <arch> is "arm64" or "arm".
#
# KUnit: the script then runs "make kunit-all" in the repo this script
# belongs to, with KERNELS set to KERNEL_ROOT. That builds a User-Mode Linux
# kernel for v5.15 and for the latest tree exactly where "make kunit" keeps
# them (a symlink farm and UML build dir per tree under the repo's build/,
# see the Makefile) and runs the driver's KUnit tests once in each, so the
# first "make kunit" afterwards is incremental on either tree. "make clean"
# removes those builds; the next "make kunit" then starts from scratch.
#
# Environment knobs:
#   KERNEL_GIT_URL  repository to clone from (default: stable/linux.git on
#                   git.kernel.org)
#   LATEST_TAG      use this tag as "latest" instead of asking the remote
#   ARCHES          architectures to prepare (default: "arm64 arm"); an arch
#                   whose cross-compiler is missing is skipped with a warning
#   KUNIT           1 (default) runs "make kunit-all" once, 0 skips it
#   FULL_BUILD      1 builds the whole kernel instead of only modules_prepare,
#                   which also creates Module.symvers (default: 0)
#   JOBS            parallel make jobs (default: nproc)

set -euo pipefail

ROOT=${1:-${KERNELS:-}}
KERNEL_GIT_URL=${KERNEL_GIT_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}
# Fixed trees, besides latest; each tree is named after its tag
PINNED_TAGS="v5.15 v6.4"
ARCHES=${ARCHES:-arm64 arm}
KUNIT=${KUNIT:-1}
FULL_BUILD=${FULL_BUILD:-0}
JOBS=${JOBS:-$(nproc)}

log() { printf '[setup-kernels] %s\n' "$*"; }
warn() { printf '[setup-kernels] WARNING: %s\n' "$*" >&2; }
die() { printf '[setup-kernels] ERROR: %s\n' "$*" >&2; exit 1; }

cross_compile_for() {
	case "$1" in
	arm64) echo aarch64-linux-gnu- ;;
	arm) echo arm-linux-gnueabihf- ;;
	*) die "unknown arch $1" ;;
	esac
}

defconfig_for() {
	case "$1" in
	arm64) echo defconfig ;;
	arm) echo multi_v7_defconfig ;;
	*) die "unknown arch $1" ;;
	esac
}

# Newest release tag on the remote, ignoring -rc tags. Nothing is pinned.
latest_remote_tag() {
	git ls-remote --tags --refs "$KERNEL_GIT_URL" 'v*' |
		sed -n 's|.*refs/tags/||p' |
		grep -E '^v[0-9]+\.[0-9]+(\.[0-9]+)?$' |
		sort -V | tail -n 1
}

# Shallow clone of one tag. Clones into a temporary dir first, so an
# interrupted clone is never mistaken for a finished one.
clone_tag() {
	local tag=$1 dst=$ROOT/src/$1

	if [ -f "$dst/Makefile" ]; then
		log "src/$tag already cloned"
		return
	fi
	log "cloning $tag from $KERNEL_GIT_URL"
	rm -rf "$dst.tmp"
	git -c advice.detachedHead=false clone --quiet --depth 1 \
		--branch "$tag" "$KERNEL_GIT_URL" "$dst.tmp"
	mv "$dst.tmp" "$dst"
}

# defconfig + modules_prepare for one tree and arch. A stamp file records the
# tag the dir was prepared from, so a newer latest tag redoes the dir.
prepare_tree() {
	local tree=$1 tag=$2 arch=$3
	local src=$ROOT/src/$tag bdir=$ROOT/build/$tree-$arch
	local cc stamp=$ROOT/build/$tree-$arch/.poc-prepared
	local want="$tag $arch full=$FULL_BUILD"

	cc=$(cross_compile_for "$arch")
	if ! command -v "${cc}gcc" >/dev/null; then
		warn "${cc}gcc not found, skipping $tree-$arch"
		return
	fi
	if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
		log "build/$tree-$arch already prepared ($tag)"
		return
	fi

	log "preparing build/$tree-$arch from $tag"
	rm -rf "$bdir"
	mkdir -p "$bdir"
	local kmake=(make -C "$src" O="$bdir" ARCH="$arch" CROSS_COMPILE="$cc" -j"$JOBS")

	"${kmake[@]}" "$(defconfig_for "$arch")" >/dev/null
	# The driver needs loadable modules and debugfs. Both are on in these
	# defconfigs today; enforce them so a defconfig change can't break us.
	"$src/scripts/config" --file "$bdir/.config" \
		--enable MODULES --enable DEBUG_FS
	"${kmake[@]}" olddefconfig >/dev/null
	# REGMAP_MMIO has no prompt; a driver selects it. Report if no
	# built-in driver does, since the module then can't be loaded there.
	grep -q '^CONFIG_REGMAP_MMIO=y' "$bdir/.config" ||
		warn "$tree-$arch: CONFIG_REGMAP_MMIO is not built in"

	if [ "$FULL_BUILD" = 1 ]; then
		"${kmake[@]}" >/dev/null
	else
		"${kmake[@]}" modules_prepare >/dev/null
	fi
	echo "$want" >"$stamp"
	log "build/$tree-$arch ready"
}

# KUnit under User-Mode Linux: one "make kunit-all" in this repo (v5.15 and
# the latest tree), so the UML builds it leaves are the ones every later
# "make kunit" reuses. Nothing is built here that the Makefile doesn't use.
# When the builds are already there this is an incremental build plus a
# test run per tree. A failing run (for example a test the working copy
# breaks) is only a warning: the trees are ready either way, and whatever
# was built is kept.
prepare_kunit() {
	local repo out

	if [ "$(uname -m)" != x86_64 ]; then
		warn "User-Mode Linux needs an x86_64 host, skipping KUnit setup"
		return
	fi
	repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
	if [ ! -f "$repo/Makefile" ]; then
		warn "no Makefile in $repo, skipping KUnit setup"
		return
	fi

	log "running make kunit-all in $repo (builds User-Mode Linux the first time)"
	out=$(mktemp)
	if make -C "$repo" kunit-all KERNELS="$ROOT" \
		KUNIT_ARGS="--jobs=$JOBS" >"$out" 2>&1; then
		log "make kunit-all passed, UML builds ready"
	else
		warn "make kunit-all failed, last lines:"
		tail -n 20 "$out" >&2
	fi
	rm -f "$out"
}

main() {
	local latest old tag

	[ -n "$ROOT" ] || die "usage: $0 KERNEL_ROOT (or set KERNELS)"
	mkdir -p "$ROOT/src" "$ROOT/build"
	log "kernel root: $ROOT"

	if [ -n "${LATEST_TAG:-}" ]; then
		latest=$LATEST_TAG
	elif ! latest=$(latest_remote_tag) || [ -z "$latest" ]; then
		# Offline: keep whatever latest tree we already have.
		latest=$(readlink "$ROOT/src/latest" 2>/dev/null) ||
			die "cannot reach $KERNEL_GIT_URL and no latest tree yet"
		warn "cannot reach $KERNEL_GIT_URL, keeping $latest"
	fi
	log "pinned: $PINNED_TAGS, latest stable: $latest"

	for tag in $PINNED_TAGS; do
		clone_tag "$tag"
	done
	clone_tag "$latest"

	# Point src/latest at the new tag and drop the tree it replaces.
	old=$(readlink "$ROOT/src/latest" 2>/dev/null || true)
	if [ "$old" != "$latest" ]; then
		ln -sfn "$latest" "$ROOT/src/latest"
		if [ -n "$old" ] && [[ " $PINNED_TAGS " != *" $old "* ]]; then
			log "removing old latest tree src/$old"
			rm -rf "${ROOT:?}/src/$old"
		fi
	fi

	# No "|| ..." here: set -e must stay active inside these functions,
	# so a failed step never writes its stamp.
	for arch in $ARCHES; do
		for tag in $PINNED_TAGS; do
			prepare_tree "$tag" "$tag" "$arch"
		done
		prepare_tree latest "$latest" "$arch"
	done

	if [ "$KUNIT" = 1 ]; then
		prepare_kunit
	fi
	log "done"
}

main "$@"
