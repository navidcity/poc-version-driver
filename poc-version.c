// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the PoC Version Register IP core (PoC.axi4lite_GitVersionRegister).
 *
 * Every Instance exposes its Values as read-only sysfs files grouped by
 * Category (common/, top/, uid/). The Values are read from the regmap cache
 * (the Cached copy). The sysfs ABI is read-only (docs/adr/0003).
 *
 * debugfs has one subdirectory per Instance under poc-version/. It shows the
 * Raw values straight from the hardware, offers a Reread trigger and, in
 * Injection builds only, allows Injection into the Cached copy for testing.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

/*
 * Standard module information
 */
#define DRIVER_NAME "poc-version"

/*
 * Kernel version guards (ADR-0002). LINUX_VERSION_MAJOR and _PATCHLEVEL exist
 * from 5.12, so on every supported kernel.
 */
#define POC_VERSION_KERNEL_AT_LEAST(_major, _patchlevel) \
	(KERNEL_VERSION(LINUX_VERSION_MAJOR, LINUX_VERSION_PATCHLEVEL, 0) >= \
	 KERNEL_VERSION(_major, _patchlevel, 0))

/*
 * Register definitions
 */

/*
 * Common.BuildDate
 */
#define POC_REG_VERSION_COMMON_BUILD_DATE_OFFSET 0x000
#define POC_REG_VERSION_COMMON_BUILD_DATE_DAY_MASK 0xFF000000
#define POC_REG_VERSION_COMMON_BUILD_DATE_MONTH_MASK 0xFF0000
#define POC_REG_VERSION_COMMON_BUILD_DATE_YEAR_MASK 0xFFFF

/*
 * Common.NumberModule_VersionOfVersionReg
 */
#define POC_REG_VERSION_COMMON_NUM_MOD_VERSION_OFFSET 0x004
#define POC_REG_VERSION_COMMON_NUM_MOD_VERSION_VERSION_MASK 0xFF

/*
 * Common.VivadoVersion
 */
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_OFFSET 0x008
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_MAJOR_MASK 0xFFFF0000
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_MINOR_MASK 0xFF00
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_PATCH_MASK 0xFF

/*
 * Common.ProjectName
 */
#define POC_REG_VERSION_COMMON_PROJECT_NAME_OFFSET 0x00C
#define POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH 20

/*
 * Top.Version
 */
#define POC_REG_VERSION_TOP_VERSION_OFFSET 0x020
#define POC_REG_VERSION_TOP_VERSION_MAJOR_MASK 0xFF000000
#define POC_REG_VERSION_TOP_VERSION_MINOR_MASK 0xFF0000
#define POC_REG_VERSION_TOP_VERSION_PATCH_MASK 0xFF00
#define POC_REG_VERSION_TOP_VERSION_COMMITS_TO_TAG_MASK 0xFC
#define POC_REG_VERSION_TOP_VERSION_UNTRACKED_MASK 0x2
#define POC_REG_VERSION_TOP_VERSION_MODIFIED_MASK 0x1

/*
 * Top.GitHash
 */
#define POC_REG_VERSION_TOP_GIT_HASH_OFFSET 0x024
#define POC_REG_VERSION_TOP_GIT_HASH_LENGTH 20

/*
 * Top.GitDate
 */
#define POC_REG_VERSION_TOP_GIT_DATE_OFFSET 0x038
#define POC_REG_VERSION_TOP_GIT_DATE_DAY_MASK 0xFF000000
#define POC_REG_VERSION_TOP_GIT_DATE_MONTH_MASK 0xFF0000
#define POC_REG_VERSION_TOP_GIT_DATE_YEAR_MASK 0xFFFF

/*
 * Top.GitTime
 */
#define POC_REG_VERSION_TOP_GIT_TIME_OFFSET 0x03C
#define POC_REG_VERSION_TOP_GIT_TIME_HOUR_MASK 0xFF000000
#define POC_REG_VERSION_TOP_GIT_TIME_MIN_MASK 0xFF0000
#define POC_REG_VERSION_TOP_GIT_TIME_SEC_MASK 0xFF00
#define POC_REG_VERSION_TOP_GIT_TIME_TIME_ZONE_MASK 0xFF

/*
 * Top.BranchName_Tag
 */
#define POC_REG_VERSION_TOP_BRANCH_NAME_TAG_OFFSET 0x040
#define POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH 64

/*
 * Top.GitURL
 */
#define POC_REG_VERSION_TOP_GIT_URL_OFFSET 0x080
#define POC_REG_VERSION_TOP_GIT_URL_LENGTH 128

/*
 * UID.UID
 */
#define POC_REG_VERSION_UID_UID_OFFSET 0x100
#define POC_REG_VERSION_UID_UID_LENGTH 16

/*
 * UID.User_eFuse
 */
#define POC_REG_VERSION_UID_USER_EFUSE_OFFSET 0x110
#define POC_REG_VERSION_UID_USER_EFUSE_LENGTH 4

/*
 * UID.User_ID
 */
#define POC_REG_VERSION_UID_USER_ID_OFFSET 0x114
#define POC_REG_VERSION_UID_USER_ID_LENGTH 12

#define POC_REG_VERSION_WORD_SIZE sizeof(u32)
/* Top.GitURL is the widest Register block */
#define POC_REG_VERSION_MAX_BLOCK_WORDS \
	(POC_REG_VERSION_TOP_GIT_URL_LENGTH / POC_REG_VERSION_WORD_SIZE)

/*
 * Register layouts
 *
 * A Register layout is pure data: its Register blocks and its Values. The
 * show, debugfs and regmap code only walk these tables, so a new layout is
 * described here without touching that code.
 *
 * Packing assumptions, not yet checked on hardware (the board script does):
 * - Strings: the first character is in the least significant byte of the
 *   Register at the lowest offset.
 * - Hex integers: the Register at the lowest offset holds the least
 *   significant word.
 * - Top.GitHash: stored with both the Register order and the byte order
 *   within each Register reversed, i.e. as a 160-bit integer in the same
 *   word order as the other hex integers.
 * - Dates and times are binary, not BCD.
 */

/*
 * Register block: one or more consecutive Registers sharing a name in the
 * IP core documentation. Single Registers are blocks of one word.
 */
struct poc_version_block {
	const char *name;
	unsigned int offset;
	unsigned int nwords;
};

/*
 * Every formatter takes the Registers of one block, lowest offset first, and
 * emits the Formatted value followed by a newline. Formatters are pure: no
 * device or regmap access.
 */
typedef ssize_t (*poc_version_format_fn)(const u32 *words, unsigned int nwords,
					 char *buf);

/* Categories, one sysfs directory each */
enum poc_version_category {
	POC_VERSION_CATEGORY_COMMON,
	POC_VERSION_CATEGORY_TOP,
	POC_VERSION_CATEGORY_UID,
};

/* Value: one sysfs file, rendered from one Register block */
struct poc_version_value {
	enum poc_version_category category;
	/* sysfs file name in the Category directory */
	const char *name;
	const struct poc_version_block *block;
	poc_version_format_fn format;
};

struct poc_version_layout {
	/* Version-of-Version-Reg that selects this layout */
	unsigned int version;
	const struct poc_version_block *blocks;
	unsigned int nblocks;
	const struct poc_version_value *values;
	unsigned int nvalues;
};

/*
 * Formatting
 */

/* Shared by Common.BuildDate and Top.GitDate */
static_assert(POC_REG_VERSION_COMMON_BUILD_DATE_DAY_MASK ==
	      POC_REG_VERSION_TOP_GIT_DATE_DAY_MASK);
static_assert(POC_REG_VERSION_COMMON_BUILD_DATE_MONTH_MASK ==
	      POC_REG_VERSION_TOP_GIT_DATE_MONTH_MASK);
static_assert(POC_REG_VERSION_COMMON_BUILD_DATE_YEAR_MASK ==
	      POC_REG_VERSION_TOP_GIT_DATE_YEAR_MASK);

/* ISO 8601 date: YYYY-MM-DD, from binary (not BCD) fields */
static ssize_t poc_version_format_date(const u32 *words, unsigned int nwords,
				       char *buf)
{
	/* Raw %ptR: year and month are taken as they are, without offsets */
	struct rtc_time tm = {
		.tm_year = FIELD_GET(POC_REG_VERSION_COMMON_BUILD_DATE_YEAR_MASK,
				     words[0]),
		.tm_mon = FIELD_GET(POC_REG_VERSION_COMMON_BUILD_DATE_MONTH_MASK,
				    words[0]),
		.tm_mday = FIELD_GET(POC_REG_VERSION_COMMON_BUILD_DATE_DAY_MASK,
				     words[0]),
	};

	return sysfs_emit(buf, "%ptRdr\n", &tm);
}

/*
 * ISO 8601 time with a whole-hour UTC offset: HH:MM:SS±hh, from binary (not
 * BCD) fields. The time zone is a signed byte in whole hours.
 */
static ssize_t poc_version_format_time(const u32 *words, unsigned int nwords,
				       char *buf)
{
	struct rtc_time tm = {
		.tm_hour = FIELD_GET(POC_REG_VERSION_TOP_GIT_TIME_HOUR_MASK,
				     words[0]),
		.tm_min = FIELD_GET(POC_REG_VERSION_TOP_GIT_TIME_MIN_MASK,
				    words[0]),
		.tm_sec = FIELD_GET(POC_REG_VERSION_TOP_GIT_TIME_SEC_MASK,
				    words[0]),
	};
	s8 zone = FIELD_GET(POC_REG_VERSION_TOP_GIT_TIME_TIME_ZONE_MASK,
			    words[0]);

	return sysfs_emit(buf, "%ptRt%+03d\n", &tm, zone);
}

/*
 * Hex integer without "0x". The lowest offset holds the least significant
 * word, so the words are printed from the highest offset down.
 *
 * Top.GitHash uses the same routine: the IP core stores it with both the
 * register order and the byte order within each register reversed, which
 * this undoes.
 */
static ssize_t poc_version_format_hex(const u32 *words, unsigned int nwords,
				      char *buf)
{
	ssize_t len = 0;
	unsigned int i;

	for (i = nwords; i-- > 0;)
		len += sysfs_emit_at(buf, len, "%08x", words[i]);
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

/*
 * Null-padded string. The first character is in the least significant byte
 * of the Register at the lowest offset. Never reads beyond the block.
 */
static ssize_t poc_version_format_string(const u32 *words, unsigned int nwords,
					 char *buf)
{
	__le32 str[POC_REG_VERSION_MAX_BLOCK_WORDS];
	size_t size = nwords * POC_REG_VERSION_WORD_SIZE;
	unsigned int i;

	if (nwords > ARRAY_SIZE(str))
		return -EINVAL;

	for (i = 0; i < nwords; i++)
		str[i] = cpu_to_le32(words[i]);

	return sysfs_emit(buf, "%.*s\n", (int)strnlen((char *)str, size),
			  (char *)str);
}

static ssize_t poc_version_format_version_of_version_reg(const u32 *words,
							 unsigned int nwords,
							 char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  FIELD_GET(POC_REG_VERSION_COMMON_NUM_MOD_VERSION_VERSION_MASK,
				    words[0]));
}

/* Major.Minor.Patch */
static ssize_t poc_version_format_vivado_version(const u32 *words,
						 unsigned int nwords,
						 char *buf)
{
	return sysfs_emit(buf, "%u.%u.%u\n",
			  FIELD_GET(POC_REG_VERSION_COMMON_VIVADO_VERSION_MAJOR_MASK,
				    words[0]),
			  FIELD_GET(POC_REG_VERSION_COMMON_VIVADO_VERSION_MINOR_MASK,
				    words[0]),
			  FIELD_GET(POC_REG_VERSION_COMMON_VIVADO_VERSION_PATCH_MASK,
				    words[0]));
}

/*
 * Major.Minor.Patch[+CommitsToTag][ dirty][ untracked], where " dirty" comes
 * from Dirty_modified and " untracked" from Dirty_untracked
 */
static ssize_t poc_version_format_top_version(const u32 *words,
					      unsigned int nwords, char *buf)
{
	u32 commits = FIELD_GET(POC_REG_VERSION_TOP_VERSION_COMMITS_TO_TAG_MASK,
				words[0]);
	ssize_t len;

	len = sysfs_emit(buf, "%u.%u.%u",
			 FIELD_GET(POC_REG_VERSION_TOP_VERSION_MAJOR_MASK, words[0]),
			 FIELD_GET(POC_REG_VERSION_TOP_VERSION_MINOR_MASK, words[0]),
			 FIELD_GET(POC_REG_VERSION_TOP_VERSION_PATCH_MASK, words[0]));
	if (commits)
		len += sysfs_emit_at(buf, len, "+%u", commits);
	if (words[0] & POC_REG_VERSION_TOP_VERSION_MODIFIED_MASK)
		len += sysfs_emit_at(buf, len, " dirty");
	if (words[0] & POC_REG_VERSION_TOP_VERSION_UNTRACKED_MASK)
		len += sysfs_emit_at(buf, len, " untracked");
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

/*
 * Register layout 1
 *
 * The blocks cover 0x000 to 0x11C without gaps or overlaps. Earlier tables
 * listed Top.GitURL21 twice (instead of Top.GitURL31) and gave UID.UID five
 * words, the last one overlapping UID.User_eFuse; both are fixed here.
 */
enum poc_version_v1_block_id {
	POC_VERSION_V1_COMMON_BUILD_DATE,
	POC_VERSION_V1_COMMON_NUM_MOD_VERSION,
	POC_VERSION_V1_COMMON_VIVADO_VERSION,
	POC_VERSION_V1_COMMON_PROJECT_NAME,
	POC_VERSION_V1_TOP_VERSION,
	POC_VERSION_V1_TOP_GIT_HASH,
	POC_VERSION_V1_TOP_GIT_DATE,
	POC_VERSION_V1_TOP_GIT_TIME,
	POC_VERSION_V1_TOP_BRANCH_NAME_TAG,
	POC_VERSION_V1_TOP_GIT_URL,
	POC_VERSION_V1_UID_UID,
	POC_VERSION_V1_UID_USER_EFUSE,
	POC_VERSION_V1_UID_USER_ID,
	POC_VERSION_V1_NUM_BLOCKS,
};

#define POC_VERSION_BLOCK(_id, _name, _offset, _length) \
	[_id] = { \
		.name = (_name), \
		.offset = (_offset), \
		.nwords = (_length) / POC_REG_VERSION_WORD_SIZE, \
	}

static const struct poc_version_block poc_version_v1_blocks[POC_VERSION_V1_NUM_BLOCKS] = {
	POC_VERSION_BLOCK(POC_VERSION_V1_COMMON_BUILD_DATE, "Common.BuildDate",
			  POC_REG_VERSION_COMMON_BUILD_DATE_OFFSET,
			  POC_REG_VERSION_WORD_SIZE),
	POC_VERSION_BLOCK(POC_VERSION_V1_COMMON_NUM_MOD_VERSION,
			  "Common.NumberModule_VersionOfVersionReg",
			  POC_REG_VERSION_COMMON_NUM_MOD_VERSION_OFFSET,
			  POC_REG_VERSION_WORD_SIZE),
	POC_VERSION_BLOCK(POC_VERSION_V1_COMMON_VIVADO_VERSION,
			  "Common.VivadoVersion",
			  POC_REG_VERSION_COMMON_VIVADO_VERSION_OFFSET,
			  POC_REG_VERSION_WORD_SIZE),
	POC_VERSION_BLOCK(POC_VERSION_V1_COMMON_PROJECT_NAME,
			  "Common.ProjectName",
			  POC_REG_VERSION_COMMON_PROJECT_NAME_OFFSET,
			  POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH),
	POC_VERSION_BLOCK(POC_VERSION_V1_TOP_VERSION, "Top.Version",
			  POC_REG_VERSION_TOP_VERSION_OFFSET,
			  POC_REG_VERSION_WORD_SIZE),
	POC_VERSION_BLOCK(POC_VERSION_V1_TOP_GIT_HASH, "Top.GitHash",
			  POC_REG_VERSION_TOP_GIT_HASH_OFFSET,
			  POC_REG_VERSION_TOP_GIT_HASH_LENGTH),
	POC_VERSION_BLOCK(POC_VERSION_V1_TOP_GIT_DATE, "Top.GitDate",
			  POC_REG_VERSION_TOP_GIT_DATE_OFFSET,
			  POC_REG_VERSION_WORD_SIZE),
	POC_VERSION_BLOCK(POC_VERSION_V1_TOP_GIT_TIME, "Top.GitTime",
			  POC_REG_VERSION_TOP_GIT_TIME_OFFSET,
			  POC_REG_VERSION_WORD_SIZE),
	POC_VERSION_BLOCK(POC_VERSION_V1_TOP_BRANCH_NAME_TAG,
			  "Top.BranchName_Tag",
			  POC_REG_VERSION_TOP_BRANCH_NAME_TAG_OFFSET,
			  POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH),
	POC_VERSION_BLOCK(POC_VERSION_V1_TOP_GIT_URL, "Top.GitURL",
			  POC_REG_VERSION_TOP_GIT_URL_OFFSET,
			  POC_REG_VERSION_TOP_GIT_URL_LENGTH),
	POC_VERSION_BLOCK(POC_VERSION_V1_UID_UID, "UID.UID",
			  POC_REG_VERSION_UID_UID_OFFSET,
			  POC_REG_VERSION_UID_UID_LENGTH),
	POC_VERSION_BLOCK(POC_VERSION_V1_UID_USER_EFUSE, "UID.User_eFuse",
			  POC_REG_VERSION_UID_USER_EFUSE_OFFSET,
			  POC_REG_VERSION_UID_USER_EFUSE_LENGTH),
	POC_VERSION_BLOCK(POC_VERSION_V1_UID_USER_ID, "UID.User_ID",
			  POC_REG_VERSION_UID_USER_ID_OFFSET,
			  POC_REG_VERSION_UID_USER_ID_LENGTH),
};

#define POC_VERSION_VALUE(_category, _name, _blocks, _block, _format) \
	{ \
		.category = POC_VERSION_CATEGORY_##_category, \
		.name = (_name), \
		.block = &(_blocks)[_block], \
		.format = (_format), \
	}

static const struct poc_version_value poc_version_v1_values[] = {
	POC_VERSION_VALUE(COMMON, "build_date", poc_version_v1_blocks,
			  POC_VERSION_V1_COMMON_BUILD_DATE,
			  poc_version_format_date),
	POC_VERSION_VALUE(COMMON, "version_of_version_reg",
			  poc_version_v1_blocks,
			  POC_VERSION_V1_COMMON_NUM_MOD_VERSION,
			  poc_version_format_version_of_version_reg),
	POC_VERSION_VALUE(COMMON, "vivado_version", poc_version_v1_blocks,
			  POC_VERSION_V1_COMMON_VIVADO_VERSION,
			  poc_version_format_vivado_version),
	POC_VERSION_VALUE(COMMON, "project_name", poc_version_v1_blocks,
			  POC_VERSION_V1_COMMON_PROJECT_NAME,
			  poc_version_format_string),
	POC_VERSION_VALUE(TOP, "version", poc_version_v1_blocks,
			  POC_VERSION_V1_TOP_VERSION,
			  poc_version_format_top_version),
	POC_VERSION_VALUE(TOP, "git_hash", poc_version_v1_blocks,
			  POC_VERSION_V1_TOP_GIT_HASH, poc_version_format_hex),
	POC_VERSION_VALUE(TOP, "git_date", poc_version_v1_blocks,
			  POC_VERSION_V1_TOP_GIT_DATE, poc_version_format_date),
	POC_VERSION_VALUE(TOP, "git_time", poc_version_v1_blocks,
			  POC_VERSION_V1_TOP_GIT_TIME, poc_version_format_time),
	POC_VERSION_VALUE(TOP, "branch_name_tag", poc_version_v1_blocks,
			  POC_VERSION_V1_TOP_BRANCH_NAME_TAG,
			  poc_version_format_string),
	POC_VERSION_VALUE(TOP, "git_url", poc_version_v1_blocks,
			  POC_VERSION_V1_TOP_GIT_URL, poc_version_format_string),
	POC_VERSION_VALUE(UID, "uid", poc_version_v1_blocks,
			  POC_VERSION_V1_UID_UID, poc_version_format_hex),
	POC_VERSION_VALUE(UID, "user_efuse", poc_version_v1_blocks,
			  POC_VERSION_V1_UID_USER_EFUSE, poc_version_format_hex),
	POC_VERSION_VALUE(UID, "user_id", poc_version_v1_blocks,
			  POC_VERSION_V1_UID_USER_ID, poc_version_format_hex),
};

static const struct poc_version_layout poc_version_layout_v1 = {
	.version = 1,
	.blocks = poc_version_v1_blocks,
	.nblocks = ARRAY_SIZE(poc_version_v1_blocks),
	.values = poc_version_v1_values,
	.nvalues = ARRAY_SIZE(poc_version_v1_values),
};

/* Every known Register layout */
static const struct poc_version_layout *const poc_version_layouts[] = {
	&poc_version_layout_v1,
};

/* Register layout for a Version-of-Version-Reg, or NULL if it is unknown */
static const struct poc_version_layout *
poc_version_layout_lookup(unsigned int version)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(poc_version_layouts); i++)
		if (poc_version_layouts[i]->version == version)
			return poc_version_layouts[i];

	return NULL;
}

/*
 * Register layout for the Raw value of Common.NumberModule_VersionOfVersionReg,
 * or NULL if it is unknown. Only the low byte, the Version-of-Version-Reg,
 * selects the layout.
 */
static const struct poc_version_layout *poc_version_layout_select(u32 raw)
{
	unsigned int version = FIELD_GET(POC_REG_VERSION_COMMON_NUM_MOD_VERSION_VERSION_MASK,
				     raw);

	return poc_version_layout_lookup(version);
}

/* Number of Registers, i.e. words, in all blocks of a layout */
static unsigned int poc_version_layout_nregs(const struct poc_version_layout *layout)
{
	unsigned int b, n = 0;

	for (b = 0; b < layout->nblocks; b++)
		n += layout->blocks[b].nwords;

	return n;
}

/* Offset of the layout's highest Register */
static unsigned int
poc_version_layout_last_offset(const struct poc_version_layout *layout)
{
	unsigned int b, end, last = 0;

	for (b = 0; b < layout->nblocks; b++) {
		end = layout->blocks[b].offset +
		      (layout->blocks[b].nwords - 1) * POC_REG_VERSION_WORD_SIZE;
		last = max(last, end);
	}

	return last;
}

/* The layout's Value for a sysfs file, or NULL if the layout lacks it */
static const struct poc_version_value *
poc_version_layout_find_value(const struct poc_version_layout *layout,
			      enum poc_version_category category,
			      const char *name)
{
	unsigned int i;

	for (i = 0; i < layout->nvalues; i++)
		if (layout->values[i].category == category &&
		    !strcmp(layout->values[i].name, name))
			return &layout->values[i];

	return NULL;
}

/*
 * Whether the Cached copy may be written at @reg. Only Injection ever writes,
 * so a build without it has no writeable Register at all (docs/adr/0003).
 * With Injection, exactly the Registers of @layout are writeable. Pure, for
 * KUnit.
 */
#ifdef POC_VERSION_INJECTION
static bool poc_version_reg_writeable(const struct poc_version_layout *layout,
				      unsigned int reg)
{
	unsigned int b;

	if (!layout || !IS_ALIGNED(reg, POC_REG_VERSION_WORD_SIZE))
		return false;

	for (b = 0; b < layout->nblocks; b++) {
		const struct poc_version_block *block = &layout->blocks[b];

		if (reg >= block->offset &&
		    reg < block->offset +
			  block->nwords * POC_REG_VERSION_WORD_SIZE)
			return true;
	}

	return false;
}
#else
static bool poc_version_reg_writeable(const struct poc_version_layout *layout,
				      unsigned int reg)
{
	return false;
}
#endif /* POC_VERSION_INJECTION */

/*
 * Exact text of a Value file, given the Raw values of all Registers
 * (@regs[i] is the Register at offset i * 4). Pure: no device access, so
 * KUnit renders through here without hardware. @buf is a sysfs page.
 */
static ssize_t __maybe_unused
poc_version_render(const struct poc_version_layout *layout, const u32 *regs,
		   size_t nregs, enum poc_version_category category,
		   const char *name, char *buf)
{
	const struct poc_version_value *value;
	unsigned int first;

	value = poc_version_layout_find_value(layout, category, name);
	if (!value)
		return -ENOENT;

	first = value->block->offset / POC_REG_VERSION_WORD_SIZE;
	if (first + value->block->nwords > nregs)
		return -EINVAL;

	return value->format(regs + first, value->block->nwords, buf);
}

struct poc_version_dev;

#ifdef POC_VERSION_INJECTION
/* Per-Register handle behind each debugfs Injection file */
struct poc_version_debugfs_reg {
	struct poc_version_dev *priv;
	unsigned int offset;
};
#endif

struct poc_version_dev {
	struct device *dev;
	void __iomem *regs;
	struct regmap *map;
	/* Register layout of this Instance */
	const struct poc_version_layout *layout;
	/* Serialises Reread, Injection and Value reads on the Cached copy */
	struct mutex lock;
	/* This Instance's subdirectory under poc_version_debugfs_root */
	struct dentry *debugfs_dir;
	struct debugfs_regset32 regset;
#ifdef POC_VERSION_INJECTION
	/* One per Register of the layout */
	struct poc_version_debugfs_reg *debugfs_regs;
#endif
};

/* Per-driver debugfs directory, alive from module init to module exit */
static struct dentry *poc_version_debugfs_root;

/*
 * Reads only Common.NumberModule_VersionOfVersionReg, before the Register
 * layout is known. Uncached, not attached to the device, and removed again
 * as soon as probe has read it.
 */
static const struct regmap_config poc_version_vovr_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = POC_REG_VERSION_WORD_SIZE,
	.max_register = POC_REG_VERSION_COMMON_NUM_MOD_VERSION_OFFSET,
	.cache_type = REGCACHE_NONE,
};

/* regmap writeable_reg callback; drvdata is unset until probe succeeded */
static bool poc_version_writeable_reg(struct device *dev, unsigned int reg)
{
	struct poc_version_dev *priv = dev_get_drvdata(dev);

	return priv && poc_version_reg_writeable(priv->layout, reg);
}

/*
 * The maple-tree cache arrived in 6.4; older kernels use the rbtree cache,
 * which also supports regcache_drop_region() (ADR-0002)
 */
#if POC_VERSION_KERNEL_AT_LEAST(6, 4)
#define POC_VERSION_REGCACHE REGCACHE_MAPLE
#else
#define POC_VERSION_REGCACHE REGCACHE_RBTREE
#endif

/*
 * Initializer of the Cached copy's regmap configuration, ending at the last
 * Register of the Instance's layout. Extra designated initializers go last;
 * KUnit adds a reg_read callback there to put the Cached copy over memory.
 *
 * The IP core is read-only. Without Injection no Register is writeable, so
 * regmap refuses every write. Reads still fill the cache: regmap caches
 * every readable, non-volatile Register, writeable or not. With Injection
 * the layout's Registers are writeable so that Injection can write into the
 * cache. Only ever write with cache-only mode switched on, and never call
 * regcache_sync(). See docs/adr/0001.
 */
#define POC_VERSION_CACHED_COPY_CONFIG(_layout, ...) { \
	.reg_bits = 32, \
	.val_bits = 32, \
	.reg_stride = POC_REG_VERSION_WORD_SIZE, \
	.max_register = poc_version_layout_last_offset(_layout), \
	.writeable_reg = poc_version_writeable_reg, \
	.cache_type = POC_VERSION_REGCACHE, \
	__VA_ARGS__ \
}

/* The regmap of the Cached copy, over the Instance's MMIO */
static struct regmap *poc_version_init_cached_copy(struct poc_version_dev *priv)
{
	const struct regmap_config config =
		POC_VERSION_CACHED_COPY_CONFIG(priv->layout);

	return devm_regmap_init_mmio(priv->dev, priv->regs, &config);
}

/*
 * Cached copy
 */

/* Caller holds priv->lock, except during probe */
static int poc_version_reread_locked(struct poc_version_dev *priv)
{
	const struct poc_version_layout *layout = priv->layout;
	u32 words[POC_REG_VERSION_MAX_BLOCK_WORDS];
	unsigned int b;
	int ret;

	ret = regcache_drop_region(priv->map, 0,
				   poc_version_layout_last_offset(layout));
	if (ret)
		return ret;

	/*
	 * Fill the cache with every Register of the layout, block by block,
	 * so reads never miss while cache-only is on
	 */
	for (b = 0; b < layout->nblocks; b++) {
		const struct poc_version_block *block = &layout->blocks[b];

		if (block->nwords > ARRAY_SIZE(words))
			return -EINVAL;

		ret = regmap_bulk_read(priv->map, block->offset, words,
				       block->nwords);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Sysfs attributes
 *
 * The attributes are every Value file any layout may have. Which block a file
 * shows, and how, comes only from the Instance's layout.
 */

struct poc_version_value_attr {
	struct device_attribute dev_attr;
	enum poc_version_category category;
};

/*
 * Mode of a Value file for a layout: its own mode if the layout has the
 * Value, 0 (absent) if not. Without a layout no Value file is exposed.
 */
static umode_t poc_version_attr_visible(const struct poc_version_layout *layout,
					struct attribute *attr)
{
	struct poc_version_value_attr *value_attr =
		container_of(attr, struct poc_version_value_attr, dev_attr.attr);

	if (!layout ||
	    !poc_version_layout_find_value(layout, value_attr->category,
					   attr->name))
		return 0;

	return attr->mode;
}

/*
 * The driver core creates dev_groups only after probe has succeeded, so
 * drvdata, and with it the Instance's layout, is set by then (5.15 onwards).
 * A missing drvdata hides the file rather than oopsing.
 */
static umode_t poc_version_value_is_visible(struct kobject *kobj,
					    struct attribute *attr, int n)
{
	struct poc_version_dev *priv = dev_get_drvdata(kobj_to_dev(kobj));

	return poc_version_attr_visible(priv ? priv->layout : NULL, attr);
}

/* Generic show: the Value's block from the Cached copy, then its formatter */
static ssize_t poc_version_value_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct poc_version_dev *priv = dev_get_drvdata(dev);
	struct poc_version_value_attr *value_attr =
		container_of(attr, struct poc_version_value_attr, dev_attr);
	const struct poc_version_value *value;
	u32 words[POC_REG_VERSION_MAX_BLOCK_WORDS];
	int ret;

	value = poc_version_layout_find_value(priv->layout, value_attr->category,
					      attr->attr.name);
	if (!value)
		return -ENOENT;
	if (value->block->nwords > ARRAY_SIZE(words))
		return -EINVAL;

	/*
	 * A Value spans several Registers; under priv->lock it never mixes
	 * Registers from before and after a Reread or an Injection. The lock
	 * is only ever held briefly, so a plain mutex_lock() is used and a
	 * read of this read-only file never fails because of a signal.
	 */
	mutex_lock(&priv->lock);

	ret = regmap_bulk_read(priv->map, value->block->offset, words,
			       value->block->nwords);

	mutex_unlock(&priv->lock);

	if (ret)
		return ret;

	return value->format(words, value->block->nwords, buf);
}

#define POC_VERSION_VALUE_ATTR(_category, _group, _name) \
	static struct poc_version_value_attr poc_version_attr_##_group##_##_name = { \
		.dev_attr = __ATTR(_name, 0444, poc_version_value_show, NULL), \
		.category = POC_VERSION_CATEGORY_##_category, \
	}

#define POC_VERSION_VALUE_ATTR_PTR(_group, _name) \
	(&poc_version_attr_##_group##_##_name.dev_attr.attr)

/* Common */
POC_VERSION_VALUE_ATTR(COMMON, common, build_date);
POC_VERSION_VALUE_ATTR(COMMON, common, version_of_version_reg);
POC_VERSION_VALUE_ATTR(COMMON, common, vivado_version);
POC_VERSION_VALUE_ATTR(COMMON, common, project_name);

static struct attribute *poc_version_common_attrs[] = {
	POC_VERSION_VALUE_ATTR_PTR(common, build_date),
	POC_VERSION_VALUE_ATTR_PTR(common, version_of_version_reg),
	POC_VERSION_VALUE_ATTR_PTR(common, vivado_version),
	POC_VERSION_VALUE_ATTR_PTR(common, project_name),
	NULL /* sentinel */,
};

static const struct attribute_group poc_version_common_group = {
	.name = "common",
	.attrs = poc_version_common_attrs,
	.is_visible = poc_version_value_is_visible,
};

/* Top */
POC_VERSION_VALUE_ATTR(TOP, top, version);
POC_VERSION_VALUE_ATTR(TOP, top, git_hash);
POC_VERSION_VALUE_ATTR(TOP, top, git_date);
POC_VERSION_VALUE_ATTR(TOP, top, git_time);
POC_VERSION_VALUE_ATTR(TOP, top, branch_name_tag);
POC_VERSION_VALUE_ATTR(TOP, top, git_url);

static struct attribute *poc_version_top_attrs[] = {
	POC_VERSION_VALUE_ATTR_PTR(top, version),
	POC_VERSION_VALUE_ATTR_PTR(top, git_hash),
	POC_VERSION_VALUE_ATTR_PTR(top, git_date),
	POC_VERSION_VALUE_ATTR_PTR(top, git_time),
	POC_VERSION_VALUE_ATTR_PTR(top, branch_name_tag),
	POC_VERSION_VALUE_ATTR_PTR(top, git_url),
	NULL /* sentinel */,
};

static const struct attribute_group poc_version_top_group = {
	.name = "top",
	.attrs = poc_version_top_attrs,
	.is_visible = poc_version_value_is_visible,
};

/* UID */
POC_VERSION_VALUE_ATTR(UID, uid, uid);
POC_VERSION_VALUE_ATTR(UID, uid, user_efuse);
POC_VERSION_VALUE_ATTR(UID, uid, user_id);

static struct attribute *poc_version_uid_attrs[] = {
	POC_VERSION_VALUE_ATTR_PTR(uid, uid),
	POC_VERSION_VALUE_ATTR_PTR(uid, user_efuse),
	POC_VERSION_VALUE_ATTR_PTR(uid, user_id),
	NULL /* sentinel */,
};

static const struct attribute_group poc_version_uid_group = {
	.name = "uid",
	.attrs = poc_version_uid_attrs,
	.is_visible = poc_version_value_is_visible,
};

/* Only the Category directories, all read-only (docs/adr/0003) */
static const struct attribute_group *poc_version_groups[] = {
	&poc_version_common_group,
	&poc_version_top_group,
	&poc_version_uid_group,
	NULL /* sentinel */,
};

/*
 * DebugFS interface
 */

/*
 * Reread: exactly "1", with or without a trailing newline, is accepted.
 * Anything else ("0x1", "01", "+1", "1\0", ...) is rejected.
 */
static ssize_t poc_version_debugfs_reread_write(struct file *file,
						const char __user *ubuf,
						size_t count, loff_t *ppos)
{
	struct poc_version_dev *priv = file->private_data;
	char kbuf[2];
	int ret = 0;

	if (count < 1 || count > sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	if (kbuf[0] != '1' || (count == 2 && kbuf[1] != '\n'))
		return -EINVAL;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;

	ret = poc_version_reread_locked(priv);

	mutex_unlock(&priv->lock);

	if (ret)
		return ret;

	dev_dbg(priv->dev, "Cached copy re-read from the hardware\n");

	return count;
}

static const struct file_operations poc_version_debugfs_reread_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = poc_version_debugfs_reread_write,
};

#ifdef POC_VERSION_INJECTION
static int poc_version_debugfs_cached_reg32_get(void *data, u64 *val)
{
	struct poc_version_debugfs_reg *reg = data;
	unsigned int word = 0;
	int ret = 0;

	ret = regmap_read(reg->priv->map, reg->offset, &word);
	if (ret)
		return ret;

	*val = word;

	return 0;
}

/* Injection: write into the Cached copy only, never to the hardware */
static int poc_version_debugfs_cached_reg32_set(void *data, u64 val)
{
	struct poc_version_debugfs_reg *reg = data;
	struct poc_version_dev *priv = reg->priv;
	int ret = 0;

	if (val > U32_MAX)
		return -ERANGE;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;

	/*
	 * With cache bypass on (regmap's own debugfs can set it), even a
	 * cache-only write goes to the hardware, so make sure it is off. Only
	 * Injection writes, always under priv->lock, so no write ever runs with
	 * cache-only off. Reread runs with it off and reads the hardware.
	 *
	 * Known gap, Injection builds only: priv->lock does not cover regmap's
	 * own debugfs. If root sets its cache_bypass between the two calls
	 * below and regmap_write(), the Injection reaches the hardware. And an
	 * Injection leaves the cache dirty; a later cache_only Y -> N toggle
	 * there runs regcache_sync(), which writes the Cached copy to the
	 * hardware (Reread does not clear the dirty flag: regmap has no API
	 * for that on any supported kernel, only regcache_mark_dirty() to set
	 * it).
	 * Both need root to poke regmap's debugfs of an Injection build; the
	 * default build has no writeable Register, so regmap never writes.
	 */
	regcache_cache_bypass(priv->map, false);
	regcache_cache_only(priv->map, true);
	ret = regmap_write(priv->map, reg->offset, val);
	regcache_cache_only(priv->map, false);

	mutex_unlock(&priv->lock);

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(poc_version_debugfs_cached_reg32_fops,
			 poc_version_debugfs_cached_reg32_get,
			 poc_version_debugfs_cached_reg32_set, "0x%08llx\n");

/* Cached copy, one file per Register, writeable for Injection */
static void poc_version_create_debugfs_injection(struct poc_version_dev *priv,
						 const struct debugfs_reg32 *regs,
						 unsigned int nregs)
{
	struct dentry *cached_dir;
	unsigned int w;

	priv->debugfs_regs = devm_kcalloc(priv->dev, nregs,
					  sizeof(*priv->debugfs_regs),
					  GFP_KERNEL);
	if (!priv->debugfs_regs)
		return;

	cached_dir = debugfs_create_dir("cached_registers", priv->debugfs_dir);
	for (w = 0; w < nregs; w++) {
		struct poc_version_debugfs_reg *reg = &priv->debugfs_regs[w];

		reg->priv = priv;
		reg->offset = regs[w].offset;
		debugfs_create_file_unsafe(regs[w].name, 0644, cached_dir, reg,
					   &poc_version_debugfs_cached_reg32_fops);
	}
}
#else
static void poc_version_create_debugfs_injection(struct poc_version_dev *priv,
						 const struct debugfs_reg32 *regs,
						 unsigned int nregs)
{
}
#endif /* POC_VERSION_INJECTION */

/* Never fails: probe goes on without debugfs */
static void poc_version_create_debugfs_interface(struct poc_version_dev *priv)
{
	const struct poc_version_layout *layout = priv->layout;
	unsigned int nregs = poc_version_layout_nregs(layout);
	struct debugfs_reg32 *regs;
	unsigned int b, w, n = 0;

	/* No debugfs at all, e.g. without CONFIG_DEBUG_FS */
	if (IS_ERR_OR_NULL(poc_version_debugfs_root))
		return;

	regs = devm_kcalloc(priv->dev, nregs, sizeof(*regs), GFP_KERNEL);
	if (!regs)
		return;

	/* One debugfs name per Register, from the layout's blocks */
	for (b = 0; b < layout->nblocks; b++) {
		const struct poc_version_block *block = &layout->blocks[b];

		for (w = 0; w < block->nwords; w++, n++) {
			regs[n].offset = block->offset +
					 w * POC_REG_VERSION_WORD_SIZE;
			/* "Top.Version" for one word, else "Top.GitURL0" and up */
			if (block->nwords == 1)
				regs[n].name = devm_kasprintf(priv->dev,
							      GFP_KERNEL, "%s",
							      block->name);
			else
				regs[n].name = devm_kasprintf(priv->dev,
							      GFP_KERNEL,
							      "%s%u",
							      block->name, w);
			if (!regs[n].name)
				return;
		}
	}

	priv->debugfs_dir = debugfs_create_dir(dev_name(priv->dev),
					       poc_version_debugfs_root);
	if (IS_ERR(priv->debugfs_dir)) {
		dev_info(priv->dev, "DebugFS interface is not available\n");
		return;
	}

	/* Raw values straight from the hardware, bypassing regmap */
	priv->regset.regs = regs;
	priv->regset.nregs = n;
	priv->regset.base = priv->regs;
	priv->regset.dev = priv->dev;
	debugfs_create_regset32("registers", 0444, priv->debugfs_dir,
				&priv->regset);

	/* Write-only Reread trigger */
	debugfs_create_file("reread", 0200, priv->debugfs_dir, priv,
			    &poc_version_debugfs_reread_fops);

	poc_version_create_debugfs_injection(priv, regs, n);
}

/*
 * Platform Device Driver
 */

/*
 * Selects the Instance's Register layout from its Version-of-Version-Reg.
 *
 * The regmap's range depends on the layout, so the Version-of-Version-Reg,
 * at the same offset in every layout, is read through a short-lived uncached
 * regmap first. It is removed again before the Cached copy is created.
 *
 * That regmap is created without a device. With one, regmap would add a
 * devres entry for dev_get_regmap() that regmap_exit() leaves behind on 6.4
 * and older (only later kernels detach it), pointing at the freed map. Without a
 * device it has no devres entry, its debugfs directory is a "dummy<n>" one
 * that regmap_exit() removes, and it never clashes with the Cached copy's.
 * Unlike the Cached copy it ignores endianness properties in the device
 * tree and reads little-endian, regmap's MMIO default; big-endian hosts are
 * out of scope (spec).
 */
static int poc_version_select_layout(struct poc_version_dev *priv)
{
	struct regmap *map;
	unsigned int raw;
	int ret;

	map = regmap_init_mmio(NULL, priv->regs,
			       &poc_version_vovr_regmap_config);
	if (IS_ERR(map))
		return dev_err_probe(priv->dev, PTR_ERR(map),
				     "failed to init regmap\n");

	ret = regmap_read(map, POC_REG_VERSION_COMMON_NUM_MOD_VERSION_OFFSET,
			  &raw);
	regmap_exit(map);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to read Version-of-Version-Reg\n");

	priv->layout = poc_version_layout_select(raw);
	if (!priv->layout) {
		/* Never bind with a layout that may show wrong Values */
		dev_err(priv->dev,
			"unknown Register layout: Version-of-Version-Reg %u (Common.NumberModule_VersionOfVersionReg 0x%08x)\n",
			(unsigned int)FIELD_GET(POC_REG_VERSION_COMMON_NUM_MOD_VERSION_VERSION_MASK,
						raw),
			raw);
		return -ENODEV;
	}

	return 0;
}

static int poc_version_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct poc_version_dev *priv = NULL;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "failed to map registers\n");

	ret = poc_version_select_layout(priv);
	if (ret)
		return ret;

	priv->map = poc_version_init_cached_copy(priv);
	if (IS_ERR(priv->map))
		return dev_err_probe(dev, PTR_ERR(priv->map),
				     "failed to init regmap\n");

	mutex_init(&priv->lock);

	ret = poc_version_reread_locked(priv);
	if (ret) {
		mutex_destroy(&priv->lock);
		return dev_err_probe(dev, ret, "failed to read registers\n");
	}

	platform_set_drvdata(pdev, priv);

	poc_version_create_debugfs_interface(priv);

	dev_dbg(dev, "Probe successful\n");

	return 0;
}

static void poc_version_remove(struct platform_device *pdev)
{
	struct poc_version_dev *priv = platform_get_drvdata(pdev);

	/* Waits for open debugfs users, so no stale entries are left behind */
	debugfs_remove_recursive(priv->debugfs_dir);
	mutex_destroy(&priv->lock);
}

#if !POC_VERSION_KERNEL_AT_LEAST(6, 3)
/* Before 6.3, .remove returns int and there is no .remove_new */
static int poc_version_remove_int(struct platform_device *pdev)
{
	poc_version_remove(pdev);

	return 0;
}
#endif

static const struct of_device_id poc_version_of_match[] = {
	{ .compatible = "plc2,poc-git-version-register", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, poc_version_of_match);

static struct platform_driver poc_version_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = poc_version_of_match,
		.dev_groups = poc_version_groups,
	},
	.probe = poc_version_probe,
	/*
	 * .remove returns void from 6.11; 6.3 to 6.10 need .remove_new, and
	 * older kernels an int-returning .remove. Out-of-tree, so the guard
	 * checks the kernel version (ADR-0002).
	 */
#if POC_VERSION_KERNEL_AT_LEAST(6, 11)
	.remove = poc_version_remove,
#elif POC_VERSION_KERNEL_AT_LEAST(6, 3)
	.remove_new = poc_version_remove,
#else
	.remove = poc_version_remove_int,
#endif
};

static int __init poc_version_init(void)
{
	int ret = 0;

	/* Before the driver is registered, so no probe runs without it */
	poc_version_debugfs_root = debugfs_create_dir(DRIVER_NAME, NULL);

	ret = platform_driver_register(&poc_version_driver);
	if (ret)
		debugfs_remove_recursive(poc_version_debugfs_root);

	return ret;
}
module_init(poc_version_init);

static void __exit poc_version_exit(void)
{
	/* Unbinds every Instance first, removing its subdirectory */
	platform_driver_unregister(&poc_version_driver);
	debugfs_remove_recursive(poc_version_debugfs_root);
}
module_exit(poc_version_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Navid Jalali <navidcity@gmail.com>");
MODULE_DESCRIPTION("Driver for the PoC Version Register IP core");

/* KUnit tests see the static tables and the rendering seam above */
#if IS_ENABLED(CONFIG_POC_VERSION_KUNIT_TEST)
#include "poc-version-test.c"
#endif
