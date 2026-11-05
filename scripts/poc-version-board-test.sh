#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Board test for the poc-version driver (spec: Testing Decisions, seam 2).
#
# Usage: poc-version-board-test.sh <dev>
#
#   <dev> is an Instance's device name, as listed under
#   /sys/bus/platform/drivers/poc-version/ (for example "a0010000.version").
#   Without an argument, the bound Instances are listed and the script exits
#   with status 2.
#
# Needs root, a kernel with CONFIG_DEBUG_FS and a driver built with
# "make INJECTION=1". debugfs is mounted at /sys/kernel/debug if it is not
# mounted yet. Only POSIX sh and busybox-level tools are used.
#
# For one Instance, the script:
#  1. Does a Reread, then saves the Raw values from debugfs "registers"
#     (read straight from the hardware) and the text of every sysfs Value
#     file.
#  2. For each test set, injects a Raw value into every Register of the
#     Cached copy through debugfs "cached_registers/<Register>", checks that
#     each one reads back, and compares every Value file of Register
#     layout 1 byte for byte, newline included, with the expected Formatted
#     value. The Raw values and expected texts are those of the KUnit cases
#     in poc-version-test.c.
#  3. Does a Reread and checks that every Register of the Cached copy equals
#     the saved hardware Raw value and that every Value file shows the saved
#     text again. A trap does the Reread even if the script fails or is
#     interrupted, so the board is always left in its true state.
#  4. Prints the true Values of the hardware and checks them for
#     plausibility. If EXPECT_<FILE> is set in the environment (for example
#     EXPECT_GIT_HASH, EXPECT_PROJECT_NAME, EXPECT_BUILD_DATE), the Value
#     file must hold exactly that text plus a newline.
#
# Packing assumptions (spec: Further Notes; recorded in poc-version.c):
#  - String byte order: the first character is in the least significant byte
#    of the Register at the lowest offset (project_name, branch_name_tag,
#    git_url).
#  - Hex word order: the Register at the lowest offset holds the least
#    significant word, and the file prints the highest offset first (uid,
#    user_efuse, user_id).
#  - Git hash reversal: Top.GitHash is stored reversed, the last eight hex
#    digits of the commit in Top.GitHash0, so the hex integer routine prints
#    it in normal commit order (git_hash).
#  - Binary, not BCD: date and time fields are binary numbers (build_date,
#    git_date, git_time).
#
# What confirms them: steps 2 and 3 check the driver on the target (its
# formatters, Injection, Reread and the Register names in debugfs), using
# Raw values packed under the assumptions above. Only the true hardware
# Values of step 4 show whether the IP core packs its Registers that way, so
# run the script with EXPECT_GIT_HASH (the commit the bitstream was built
# from) and EXPECT_PROJECT_NAME, EXPECT_BRANCH_NAME_TAG, EXPECT_GIT_URL,
# EXPECT_BUILD_DATE, EXPECT_GIT_DATE, EXPECT_GIT_TIME and EXPECT_UID set to
# what is known about the design. Every failure names the assumption it
# points to.
#
# Do not touch regmap's own debugfs files: while this script runs, and on
# an Injection build in general, nobody may write
# /sys/kernel/debug/regmap/<dev>/cache_only or cache_bypass. Toggling them
# can make regmap sync the Cached copy, so injected Raw values may be
# written to the IP core. This script uses only the driver's own debugfs
# directory (<debugfs>/poc-version/<dev>) and never opens regmap's.
#
# Exit status: 0 when every check passed, 1 on any failure (including a
# missing debugfs, a build without Injection or an unbound device), 2 for
# usage.
#
# Testing the script itself without a board: SYSFS_ROOT (default /sys) and
# DEBUGFS_ROOT (default /sys/kernel/debug) move the trees. If either is
# set, the root check and the debugfs mount are skipped. If
# POC_VERSION_FAKE_HOOK is set, it is run as "$POC_VERSION_FAKE_HOOK inject"
# after each Injection and "$POC_VERSION_FAKE_HOOK reread" after each
# Reread, so a fake driver can update its files. Never set it on a board.

# Functions are called through variables and traps, and some variables are
# set through eval
# shellcheck disable=SC2329,SC2154

DRIVER=poc-version

TEST_MODE=0
if [ -n "${SYSFS_ROOT+x}" ] || [ -n "${DEBUGFS_ROOT+x}" ]; then
	TEST_MODE=1
fi
SYSFS_ROOT=${SYSFS_ROOT:-/sys}
DEBUGFS_ROOT=${DEBUGFS_ROOT:-/sys/kernel/debug}
DRV_DIR=$SYSFS_ROOT/bus/platform/drivers/$DRIVER

# Register blocks of layout 1 as "name:words", in offset order from 0x000.
# A one-word block is a Register of the same name; the Registers of a longer
# block are named <block>0 (lowest offset) and up, as in debugfs.
LAYOUT1_BLOCKS="Common.BuildDate:1 Common.NumberModule_VersionOfVersionReg:1
Common.VivadoVersion:1 Common.ProjectName:5 Top.Version:1 Top.GitHash:5
Top.GitDate:1 Top.GitTime:1 Top.BranchName_Tag:16 Top.GitURL:32 UID.UID:4
UID.User_eFuse:1 UID.User_ID:3"

# Every Value file of layout 1, <Category>/<file>
LAYOUT1_VALUES="common/build_date common/version_of_version_reg
common/vivado_version common/project_name top/version top/git_hash
top/git_date top/git_time top/branch_name_tag top/git_url uid/uid
uid/user_efuse uid/user_id"

FAILS=0
CHECKS=0
WARNS=0
NEED_REREAD=0

msg() {
	printf '%s\n' "$*"
}

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

fail() {
	FAILS=$((FAILS + 1))
	printf 'FAIL: %s\n' "$*"
}

warn() {
	WARNS=$((WARNS + 1))
	printf 'WARN: %s\n' "$*"
}

# Packing assumption that a wrong Value file points to
assumption_for() {
	case $1 in
	*/project_name | */branch_name_tag | */git_url)
		msg "string byte order (first character in the least significant byte of the lowest Register)" ;;
	*/git_hash)
		msg "Git hash reversal (last eight digits of the commit in Top.GitHash0) and hex word order" ;;
	*/uid | */user_efuse | */user_id)
		msg "hex word order (lowest offset is the least significant word)" ;;
	*/build_date | */git_date | */git_time)
		msg "binary, not BCD, date and time fields" ;;
	*)
		msg "none (bit fields of a single Register)" ;;
	esac
}

# Show bytes, with non-printables made visible
show_bytes() {
	if command -v od >/dev/null 2>&1; then
		printf '%s' "$1" | od -A n -c | sed 's/^/        /'
	else
		printf '%s' "$1" | sed -n 'l' | sed 's/^/        /'
	fi
}

# "Top.GitURL3" -> "Top_GitURL3", "top/git_url" -> "top_git_url"
key_of() {
	k=${1%%[./]*}_${1#*[./]}
}

# Exact text of a file, newline included: echoes it plus a sentinel "x".
# Prints nothing if the file cannot be read.
read_exact() {
	cat "$1" 2>/dev/null && printf x
}

run_hook() {
	if [ -n "${POC_VERSION_FAKE_HOOK-}" ]; then
		"$POC_VERSION_FAKE_HOOK" "$1" || die "fake hook failed"
	fi
}

do_reread() {
	printf '1\n' >"$DBG_DIR/reread" 2>/dev/null || return 1
	run_hook reread
}

cleanup() {
	status=$?
	trap - EXIT INT TERM HUP
	if [ "$NEED_REREAD" = 1 ]; then
		msg "Restoring the Cached copy with a Reread"
		if do_reread; then
			NEED_REREAD=0
		else
			printf 'ERROR: Reread failed, the Cached copy may still hold injected values; run: echo 1 > %s\n' \
				"$DBG_DIR/reread" >&2
			[ "$status" = 0 ] && status=1
		fi
	fi
	exit "$status"
}

list_instances() {
	found=0
	for d in "$DRV_DIR"/*/; do
		[ -d "${d}common" ] || continue
		d=${d%/}
		msg "  ${d##*/}"
		found=1
	done
	[ "$found" = 1 ] || msg "  (none bound)"
}

#
# Raw values to inject and expected Formatted values of the current set
#

# setreg <Register> <Raw value as 0x%08x>
setreg() {
	key_of "$1"
	eval "INJ_$k=\$2"
}

# expect <Category>/<file> <Formatted value without the newline>
expect() {
	key_of "$1"
	eval "EXP_$k=\$2"
}

clear_regs() {
	for n in $REG_NAMES; do
		setreg "$n" 0x00000000
	done
}

# pack_string <block> <words> <text>: the text, null-padded, first character
# in the least significant byte of <block>0. ASCII only.
pack_string() {
	ps_block=$1
	ps_words=$2
	ps_s=$3
	[ "${#ps_s}" -le $((ps_words * 4)) ] || die "string too long for $ps_block"
	ps_w=0
	while [ "$ps_w" -lt "$ps_words" ]; do
		ps_v=0
		ps_b=0
		while [ "$ps_b" -lt 4 ]; do
			if [ -n "$ps_s" ]; then
				ps_c=${ps_s%"${ps_s#?}"}
				ps_s=${ps_s#?}
				ps_v=$((ps_v + $(printf '%d' "'$ps_c") * (1 << (8 * ps_b))))
			fi
			ps_b=$((ps_b + 1))
		done
		setreg "$ps_block$ps_w" "$(printf '0x%08x' "$ps_v")"
		ps_w=$((ps_w + 1))
	done
}

# Typical set: one KUnit typical case per Value. Every other set starts here.
set_typical() {
	clear_regs

	setreg Common.BuildDate 0x1b0a07ea
	expect common/build_date 2026-10-27

	# Upper bits set: the file shows the low byte only
	setreg Common.NumberModule_VersionOfVersionReg 0x00030001
	expect common/version_of_version_reg 1

	setreg Common.VivadoVersion 0x07e80200
	expect common/vivado_version 2024.2.0

	# Byte order pinned with literal Raw values
	setreg Common.ProjectName0 0x625f796d
	setreg Common.ProjectName1 0x6472616f
	setreg Common.ProjectName2 0x7365645f
	setreg Common.ProjectName3 0x006e6769
	expect common/project_name my_board_design

	setreg Top.Version 0x0104000f
	expect top/version "1.4.0+3 dirty untracked"

	# Commit bc91981278b2611b0b12dd09bf1a2d8f74f0b8ed, stored reversed
	setreg Top.GitHash0 0x74f0b8ed
	setreg Top.GitHash1 0xbf1a2d8f
	setreg Top.GitHash2 0x0b12dd09
	setreg Top.GitHash3 0x78b2611b
	setreg Top.GitHash4 0xbc919812
	expect top/git_hash bc91981278b2611b0b12dd09bf1a2d8f74f0b8ed

	setreg Top.GitDate 0x190a07ea
	expect top/git_date 2026-10-25

	setreg Top.GitTime 0x0e0309fb
	expect top/git_time 14:03:09-05

	pack_string Top.BranchName_Tag 16 main
	expect top/branch_name_tag main

	pack_string Top.GitURL 32 https://git.example.org/fpga/board.git
	expect top/git_url https://git.example.org/fpga/board.git

	setreg UID.UID0 0x0c0d0e0f
	setreg UID.UID1 0x08090a0b
	setreg UID.UID2 0x04050607
	setreg UID.UID3 0x00010203
	expect uid/uid 000102030405060708090a0b0c0d0e0f

	setreg UID.User_eFuse 0xdeadbeef
	expect uid/user_efuse deadbeef

	setreg UID.User_ID0 0x89abcdef
	setreg UID.User_ID1 0x01234567
	setreg UID.User_ID2 0x00c0ffee
	expect uid/user_id 00c0ffee0123456789abcdef
}

# Full-length strings with a non-null byte right after each block, 63
# commits to tag, time zone +00, month 13, zero User eFuse
set_full_length() {
	set_typical

	setreg Common.BuildDate 0x010d07ea
	expect common/build_date 2026-13-01

	setreg Common.NumberModule_VersionOfVersionReg 0x00000001
	expect common/version_of_version_reg 1

	pack_string Common.ProjectName 5 abcdefghijklmnopqrst
	expect common/project_name abcdefghijklmnopqrst

	# Low byte 0xfc: must not leak into project_name
	setreg Top.Version 0x020a05fc
	expect top/version 2.10.5+63

	setreg Top.GitTime 0x0e030900
	expect top/git_time 14:03:09+00

	# Followed by the non-null Top.GitURL0
	fl_s=release/yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy
	pack_string Top.BranchName_Tag 16 "$fl_s"
	expect top/branch_name_tag "$fl_s"

	# Followed by UID.UID0, whose low byte 0x0f is not null
	fl_s=https://git.example.org/fpga/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.git
	pack_string Top.GitURL 32 "$fl_s"
	expect top/git_url "$fl_s"

	# Neighbours UID.UID3 and UID.User_ID0 stay non-zero
	setreg UID.User_eFuse 0x00000000
	expect uid/user_efuse 00000000
}

# Empty strings, a string padded after a null byte, time zone +01, dirty
# only, zero UID and User ID next to an all-ones User eFuse
set_empty() {
	set_typical

	setreg Common.ProjectName0 0x58006261
	setreg Common.ProjectName1 0x58585858
	setreg Common.ProjectName2 0x00000000
	setreg Common.ProjectName3 0x00000000
	setreg Common.ProjectName4 0x58585858
	expect common/project_name ab

	setreg Top.Version 0x01040001
	expect top/version "1.4.0 dirty"

	setreg Top.GitTime 0x173b3b01
	expect top/git_time 23:59:59+01

	pack_string Top.BranchName_Tag 16 ""
	expect top/branch_name_tag ""

	pack_string Top.GitURL 32 ""
	expect top/git_url ""

	pack_string UID.UID 4 ""
	expect uid/uid 00000000000000000000000000000000

	setreg UID.User_eFuse 0xffffffff
	expect uid/user_efuse ffffffff

	pack_string UID.User_ID 3 ""
	expect uid/user_id 000000000000000000000000
}

set_version_untracked() {
	set_typical
	setreg Top.Version 0x01040002
	expect top/version "1.4.0 untracked"
}

set_version_dirty_untracked() {
	set_typical
	setreg Top.Version 0x01040003
	expect top/version "1.4.0 dirty untracked"
}

set_version_commits() {
	set_typical
	setreg Top.Version 0x0104000c
	expect top/version 1.4.0+3
}

set_version_on_tag() {
	set_typical
	setreg Top.Version 0x01040000
	expect top/version 1.4.0
}

TEST_SETS="set_typical set_full_length set_empty set_version_untracked
set_version_dirty_untracked set_version_commits set_version_on_tag"

#
# Checks
#

# check_value <Category>/<file> <expected text with newline and sentinel>
check_value() {
	cv_got=$(read_exact "$DEV_DIR/$1")
	CHECKS=$((CHECKS + 1))
	[ "$cv_got" = "$2" ] && return 0
	if [ -z "$cv_got" ]; then
		fail "$1: cannot read $DEV_DIR/$1"
	else
		fail "$1"
		msg "    expected:"
		show_bytes "${2%x}"
		msg "    got:"
		show_bytes "${cv_got%x}"
	fi
	msg "    assumption: $(assumption_for "$1")"
	return 1
}

# Write every Register of the current set into the Cached copy
inject_set() {
	NEED_REREAD=1
	for n in $REG_NAMES; do
		key_of "$n"
		eval "is_v=\$INJ_$k"
		printf '%s\n' "$is_v" >"$DBG_DIR/cached_registers/$n" 2>/dev/null ||
			die "Injection into $n failed"
	done
	run_hook inject
}

run_set() {
	rs_fails=$FAILS
	"$1"
	inject_set

	for n in $REG_NAMES; do
		key_of "$n"
		eval "rs_v=\$INJ_$k"
		rs_got=$(cat "$DBG_DIR/cached_registers/$n" 2>/dev/null)
		CHECKS=$((CHECKS + 1))
		[ "$rs_got" = "$rs_v" ] ||
			fail "$1: cached_registers/$n reads '$rs_got', injected '$rs_v' (Injection into the Cached copy)"
	done

	for v in $LAYOUT1_VALUES; do
		key_of "$v"
		eval "rs_v=\$EXP_$k"
		check_value "$v" "$rs_v
x" || msg "    (test set $1)"
	done

	if [ "$FAILS" = "$rs_fails" ]; then
		msg "ok    $1"
	else
		msg "FAIL  $1: $((FAILS - rs_fails)) failure(s)"
	fi
}

# Strip leading zeros so $(( )) never sees octal
dec() {
	d=${1#"${1%%[!0]*}"}
	[ -n "$d" ] || d=0
}

# in_range <value> <min> <max>
in_range() {
	case $1 in '' | *[!0-9]*) return 1 ;; esac
	dec "$1"
	[ "$d" -ge "$2" ] && [ "$d" -le "$3" ]
}

# Plausibility of a true hardware Value. Warnings only: the driver shows
# Values as they are, but BCD dates or a swapped string show up here.
plausible() {
	pl_t=$2
	case $1 in
	*/build_date | */git_date)
		pl_y=${pl_t%%-*}
		pl_m=${pl_t#*-}
		pl_d=${pl_m#*-}
		pl_m=${pl_m%%-*}
		in_range "$pl_y" 2000 2099 && in_range "$pl_m" 1 12 &&
			in_range "$pl_d" 1 31 && return 0
		warn "$1: '$pl_t' is not a plausible date; assumption: $(assumption_for "$1")"
		;;
	*/git_time)
		pl_z=${pl_t#????????}
		pl_t=${pl_t%"$pl_z"}
		pl_h=${pl_t%%:*}
		pl_s=${pl_t##*:}
		pl_m=${pl_t#*:}
		pl_m=${pl_m%%:*}
		pl_zn=${pl_z#?}
		in_range "$pl_h" 0 23 && in_range "$pl_m" 0 59 &&
			in_range "$pl_s" 0 59 && in_range "$pl_zn" 0 14 && return 0
		warn "$1: '$2' is not a plausible time; assumption: $(assumption_for "$1")"
		;;
	*/vivado_version)
		in_range "${pl_t%%.*}" 2000 2099 ||
			warn "$1: '$pl_t' is not a plausible Vivado version"
		;;
	*/project_name | */branch_name_tag | */git_url)
		if [ -z "$pl_t" ]; then
			warn "$1: empty"
		elif printf '%s' "$pl_t" | LC_ALL=C grep -q '[^ -~]'; then
			warn "$1: has non-printable bytes; assumption: $(assumption_for "$1")"
		fi
		;;
	esac
}

#
# Main
#

if [ $# -ne 1 ] || [ -z "$1" ]; then
	msg "Usage: ${0##*/} <dev>"
	msg "Bound $DRIVER Instances:"
	list_instances
	exit 2
fi
DEV=$1
DEV_DIR=$SYSFS_ROOT/bus/platform/devices/$DEV

if [ "$TEST_MODE" = 0 ]; then
	[ "$(id -u)" = 0 ] || die "must run as root"
	if ! grep -q "^[^ ]* $DEBUGFS_ROOT debugfs " /proc/mounts; then
		mount -t debugfs debugfs "$DEBUGFS_ROOT" ||
			die "cannot mount debugfs at $DEBUGFS_ROOT (kernel without CONFIG_DEBUG_FS?)"
	fi
fi

[ -d "$DRV_DIR" ] || die "$DRV_DIR missing: the $DRIVER driver is not loaded"
if [ ! -d "$DRV_DIR/$DEV" ] || [ ! -d "$DEV_DIR/common" ]; then
	printf 'ERROR: %s is not a bound %s Instance. Bound Instances:\n' "$DEV" "$DRIVER" >&2
	list_instances >&2
	exit 1
fi

[ -d "$DEBUGFS_ROOT/$DRIVER" ] ||
	die "$DEBUGFS_ROOT/$DRIVER missing: debugfs is not mounted there, or the kernel has no CONFIG_DEBUG_FS"
DBG_DIR=$DEBUGFS_ROOT/$DRIVER/$DEV
[ -d "$DBG_DIR" ] || die "$DBG_DIR missing: probe could not create the Instance's debugfs directory"
[ -r "$DBG_DIR/registers" ] || die "$DBG_DIR/registers missing"
[ -e "$DBG_DIR/reread" ] || die "$DBG_DIR/reread missing"
[ -d "$DBG_DIR/cached_registers" ] ||
	die "$DBG_DIR/cached_registers missing: Injection is not available (driver built without 'make INJECTION=1'); nothing was tested"

VOVR=$(read_exact "$DEV_DIR/common/version_of_version_reg")
[ "$VOVR" = "1
x" ] || die "Version-of-Version-Reg is '${VOVR%?x}', this script covers Register layout 1 only"

# Register names of layout 1, as debugfs names them
REG_NAMES=
for b in $LAYOUT1_BLOCKS; do
	b_name=${b%:*}
	b_words=${b#*:}
	if [ "$b_words" = 1 ]; then
		REG_NAMES="$REG_NAMES $b_name"
	else
		w=0
		while [ "$w" -lt "$b_words" ]; do
			REG_NAMES="$REG_NAMES $b_name$w"
			w=$((w + 1))
		done
	fi
done

for n in $REG_NAMES; do
	[ -f "$DBG_DIR/cached_registers/$n" ] ||
		die "cached_registers/$n missing: the driver's Register names differ from this script's layout 1 table"
done

# Every Value file of layout 1 is there, and nothing else
for v in $LAYOUT1_VALUES; do
	[ -f "$DEV_DIR/$v" ] || die "$DEV_DIR/$v missing"
done
for c in common top uid; do
	for f in "$DEV_DIR/$c"/*; do
		case " $(printf '%s' "$LAYOUT1_VALUES" | tr '\n' ' ') " in
		*" $c/${f##*/} "*) ;;
		*) die "unexpected Value file $f: update this script's layout 1 table" ;;
		esac
	done
done

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

msg "poc-version board test of $DEV"

# 1. True state: Raw values from the hardware and the Value files
do_reread || die "Reread failed"
HW_NAMES=
while IFS= read -r line; do
	case $line in
	*" = 0x"*) ;;
	*) die "unexpected line in $DBG_DIR/registers: '$line'" ;;
	esac
	n=${line%% = *}
	key_of "$n"
	eval "HW_$k=\${line##* = }"
	HW_NAMES="$HW_NAMES $n"
done <"$DBG_DIR/registers"
[ "$HW_NAMES" = "$REG_NAMES" ] ||
	die "Registers in $DBG_DIR/registers differ from this script's layout 1 table"

for v in $LAYOUT1_VALUES; do
	key_of "$v"
	sv_t=$(read_exact "$DEV_DIR/$v")
	[ -n "$sv_t" ] || die "cannot read $DEV_DIR/$v"
	CHECKS=$((CHECKS + 1))
	case $sv_t in
	*"
x") ;;
	*) fail "$v: the hardware Value does not end with a newline" ;;
	esac
	eval "SAVED_$k=\$sv_t"
done

# 2. Injection and Value files
msg "Injecting test sets:"
for s in $TEST_SETS; do
	run_set "$s"
done

# 3. Reread: back to the hardware
msg "Reread:"
rr_fails=$FAILS
do_reread || die "Reread failed"
NEED_REREAD=0
for n in $REG_NAMES; do
	key_of "$n"
	eval "rr_hw=\$HW_$k"
	rr_got=$(cat "$DBG_DIR/cached_registers/$n" 2>/dev/null)
	CHECKS=$((CHECKS + 1))
	[ "$rr_got" = "$rr_hw" ] ||
		fail "after Reread, cached_registers/$n reads '$rr_got', hardware Raw value '$rr_hw'"
done
for v in $LAYOUT1_VALUES; do
	key_of "$v"
	eval "rr_t=\$SAVED_$k"
	check_value "$v" "$rr_t" || msg "    (after Reread, compared with the text before Injection)"
done
if [ "$FAILS" = "$rr_fails" ]; then
	msg "ok    Cached copy matches the hardware again"
else
	msg "FAIL  Reread: $((FAILS - rr_fails)) failure(s)"
fi

# 4. True Values of the hardware
msg "Hardware Values:"
for v in $LAYOUT1_VALUES; do
	key_of "$v"
	eval "hv_t=\$SAVED_$k"
	hv_t=${hv_t%?x}
	msg "  $v: $hv_t"
	plausible "$v" "$hv_t"
	hv_var=EXPECT_$(printf '%s' "${v#*/}" | tr '[:lower:]' '[:upper:]')
	eval "hv_set=\${$hv_var+x}"
	if [ -n "$hv_set" ]; then
		eval "hv_exp=\$$hv_var"
		check_value "$v" "$hv_exp
x" && msg "ok    $v matches $hv_var"
	fi
done

msg ""
if [ "$FAILS" = 0 ]; then
	msg "PASS: $CHECKS checks, $WARNS warning(s)"
	exit 0
fi
msg "FAIL: $FAILS of $CHECKS checks failed, $WARNS warning(s)"
exit 1
