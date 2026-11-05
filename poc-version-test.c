// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the poc-version driver, at the Value-rendering seam and at
 * the Register layout selection and Value file visibility seams, at the
 * regmap's writeable decision, and through regmap's cache (the Cached copy
 * filled and Reread by the driver's own code, over memory).
 *
 * Each rendering case gives the Raw values of an Instance's Registers and one
 * Value file, and checks the exact Formatted value, trailing newline included.
 * Raw values are written as the IP core would hold them under the packing
 * assumptions recorded in poc-version.c.
 *
 * Included at the end of poc-version.c, so it is built only in-tree with
 * CONFIG_POC_VERSION_KUNIT_TEST. It runs on v5.15 too, so it sticks to the
 * KUnit macros and C (gnu89) that v5.15 has: no KUNIT_*_NULL, no
 * declarations in for loops.
 */

#include <kunit/test.h>

/* Register layout 1 spans 0x000 to 0x11C */
#define POC_VERSION_TEST_NREGS (0x120 / 4)
/* Common.NumberModule_VersionOfVersionReg, the same in every layout */
#define POC_VERSION_TEST_VOVR_OFFSET 0x004
#define POC_VERSION_TEST_MAX_REGS 6

struct poc_version_render_case {
	const char *desc;
	enum poc_version_category category;
	const char *file;
	/* Raw values by offset, ORed in; unlisted Registers read zero */
	struct {
		unsigned int offset;
		u32 raw;
	} regs[POC_VERSION_TEST_MAX_REGS];
	/*
	 * Optional string, without its null byte, packed from str_offset with
	 * the first character in the least significant byte
	 */
	const char *str;
	unsigned int str_offset;
	const char *expected;
};

/* Neighbouring Register that must never leak into a string: "AAAA" */
#define POC_VERSION_TEST_LEAK 0x41414141

static const struct poc_version_render_case poc_version_render_cases[] = {
	/* Common */
	{
		.desc = "build date",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "build_date",
		.regs = { { 0x000, 0x1b0a07ea } },
		.expected = "2026-10-27\n",
	},
	{
		.desc = "build date with month 13 is printed as it is",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "build_date",
		.regs = { { 0x000, 0x010d07ea } },
		.expected = "2026-13-01\n",
	},
	{
		.desc = "version of version reg ignores the upper bits",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "version_of_version_reg",
		.regs = { { 0x004, 0x00030000 } },
		.expected = "1\n",
	},
	{
		.desc = "vivado version",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "vivado_version",
		.regs = { { 0x008, 0x07e80200 } },
		.expected = "2024.2.0\n",
	},
	{
		.desc = "project name, byte order pinned",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "project_name",
		.regs = {
			{ 0x00c, 0x625f796d },
			{ 0x010, 0x6472616f },
			{ 0x014, 0x7365645f },
			{ 0x018, 0x006e6769 },
		},
		.expected = "my_board_design\n",
	},
	{
		.desc = "project name full-length without null byte",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "project_name",
		.regs = { { 0x020, POC_VERSION_TEST_LEAK } },
		.str = "abcdefghijklmnopqrst",
		.str_offset = 0x00c,
		.expected = "abcdefghijklmnopqrst\n",
	},
	{
		.desc = "project name empty",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "project_name",
		.regs = { { 0x020, POC_VERSION_TEST_LEAK } },
		.expected = "\n",
	},
	{
		.desc = "project name stops at the first null byte",
		.category = POC_VERSION_CATEGORY_COMMON,
		.file = "project_name",
		.regs = {
			{ 0x00c, 0x58006261 },
			{ 0x010, 0x58585858 },
			{ 0x01c, 0x58585858 },
		},
		.expected = "ab\n",
	},
	/* Top.Version */
	{
		.desc = "top version on the tag",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x01040000 } },
		.expected = "1.4.0\n",
	},
	{
		.desc = "top version with commits to tag",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x0104000c } },
		.expected = "1.4.0+3\n",
	},
	{
		.desc = "top version with 63 commits to tag",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x020a05fc } },
		.expected = "2.10.5+63\n",
	},
	{
		.desc = "top version dirty",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x01040001 } },
		.expected = "1.4.0 dirty\n",
	},
	{
		.desc = "top version untracked",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x01040002 } },
		.expected = "1.4.0 untracked\n",
	},
	{
		.desc = "top version dirty and untracked",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x01040003 } },
		.expected = "1.4.0 dirty untracked\n",
	},
	{
		.desc = "top version with commits to tag, dirty and untracked",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "version",
		.regs = { { 0x020, 0x0104000f } },
		.expected = "1.4.0+3 dirty untracked\n",
	},
	/* Top.GitHash */
	{
		/*
		 * Commit bc91981278b2611b0b12dd09bf1a2d8f74f0b8ed, stored
		 * reversed: its last eight digits at the lowest offset
		 */
		.desc = "git hash of a known commit, reversal pinned",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_hash",
		.regs = {
			{ 0x024, 0x74f0b8ed },
			{ 0x028, 0xbf1a2d8f },
			{ 0x02c, 0x0b12dd09 },
			{ 0x030, 0x78b2611b },
			{ 0x034, 0xbc919812 },
		},
		.expected = "bc91981278b2611b0b12dd09bf1a2d8f74f0b8ed\n",
	},
	/* Top.GitDate and Top.GitTime */
	{
		.desc = "git date",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_date",
		.regs = { { 0x038, 0x190a07ea } },
		.expected = "2026-10-25\n",
	},
	{
		.desc = "git time zone -05",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_time",
		.regs = { { 0x03c, 0x0e0309fb } },
		.expected = "14:03:09-05\n",
	},
	{
		.desc = "git time zone +00",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_time",
		.regs = { { 0x03c, 0x0e030900 } },
		.expected = "14:03:09+00\n",
	},
	{
		.desc = "git time zone +01",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_time",
		.regs = { { 0x03c, 0x173b3b01 } },
		.expected = "23:59:59+01\n",
	},
	/* Top.BranchName_Tag */
	{
		.desc = "branch name",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "branch_name_tag",
		.str = "main",
		.str_offset = 0x040,
		.expected = "main\n",
	},
	{
		.desc = "branch name full-length without null byte",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "branch_name_tag",
		.regs = { { 0x080, POC_VERSION_TEST_LEAK } },
		.str =
		"release/yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
		.str_offset = 0x040,
		.expected =
		"release/yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\n",
	},
	{
		.desc = "branch name empty",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "branch_name_tag",
		.regs = { { 0x080, POC_VERSION_TEST_LEAK } },
		.expected = "\n",
	},
	/* Top.GitURL */
	{
		.desc = "git url",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_url",
		.str = "https://git.example.org/fpga/board.git",
		.str_offset = 0x080,
		.expected = "https://git.example.org/fpga/board.git\n",
	},
	{
		.desc = "git url full-length without null byte",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_url",
		.regs = { { 0x100, POC_VERSION_TEST_LEAK } },
		.str =
		"https://git.example.org/fpga/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.git",
		.str_offset = 0x080,
		.expected =
		"https://git.example.org/fpga/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.git\n",
	},
	{
		.desc = "git url empty",
		.category = POC_VERSION_CATEGORY_TOP,
		.file = "git_url",
		.regs = { { 0x100, POC_VERSION_TEST_LEAK } },
		.expected = "\n",
	},
	/* UID, most significant word (highest offset) first */
	{
		.desc = "uid",
		.category = POC_VERSION_CATEGORY_UID,
		.file = "uid",
		.regs = {
			{ 0x100, 0x0c0d0e0f },
			{ 0x104, 0x08090a0b },
			{ 0x108, 0x04050607 },
			{ 0x10c, 0x00010203 },
		},
		.expected = "000102030405060708090a0b0c0d0e0f\n",
	},
	{
		.desc = "uid all zero",
		.category = POC_VERSION_CATEGORY_UID,
		.file = "uid",
		.regs = { { 0x110, 0xffffffff } },
		.expected = "00000000000000000000000000000000\n",
	},
	{
		.desc = "user efuse",
		.category = POC_VERSION_CATEGORY_UID,
		.file = "user_efuse",
		.regs = { { 0x110, 0xdeadbeef } },
		.expected = "deadbeef\n",
	},
	{
		.desc = "user efuse all zero",
		.category = POC_VERSION_CATEGORY_UID,
		.file = "user_efuse",
		.regs = { { 0x10c, 0xffffffff }, { 0x114, 0xffffffff } },
		.expected = "00000000\n",
	},
	{
		.desc = "user id",
		.category = POC_VERSION_CATEGORY_UID,
		.file = "user_id",
		.regs = {
			{ 0x114, 0x89abcdef },
			{ 0x118, 0x01234567 },
			{ 0x11c, 0x00c0ffee },
		},
		.expected = "00c0ffee0123456789abcdef\n",
	},
	{
		.desc = "user id all zero",
		.category = POC_VERSION_CATEGORY_UID,
		.file = "user_id",
		.regs = { { 0x110, 0xffffffff } },
		.expected = "000000000000000000000000\n",
	},
};

static void poc_version_render_case_desc(const struct poc_version_render_case *c,
					 char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(poc_version_render, poc_version_render_cases,
		  poc_version_render_case_desc);

static void poc_version_render_test(struct kunit *test)
{
	const struct poc_version_render_case *c = test->param_value;
	u32 regs[POC_VERSION_TEST_NREGS] = { };
	const struct poc_version_layout *layout;
	unsigned int i, offset;
	ssize_t len;
	char *buf;

	/* Every case is an Instance with Register layout 1 */
	regs[POC_VERSION_TEST_VOVR_OFFSET / 4] = 1;

	for (i = 0; i < ARRAY_SIZE(c->regs); i++)
		regs[c->regs[i].offset / 4] |= c->regs[i].raw;

	for (i = 0; c->str && c->str[i]; i++) {
		offset = c->str_offset + i;
		regs[offset / 4] |= (u32)(u8)c->str[i] << (8 * (offset % 4));
	}

	layout = poc_version_layout_select(regs[POC_VERSION_TEST_VOVR_OFFSET / 4]);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, layout);

	/* sysfs_emit() wants a page-aligned page */
	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	len = poc_version_render(layout, regs, ARRAY_SIZE(regs), c->category,
				 c->file, buf);
	KUNIT_EXPECT_STREQ(test, buf, c->expected);
	KUNIT_EXPECT_EQ(test, len, (ssize_t)strlen(c->expected));
}

/*
 * Register layout selection
 */

struct poc_version_select_case {
	const char *desc;
	/* Raw value of Common.NumberModule_VersionOfVersionReg */
	u32 raw;
	/* Version of the selected layout, or 0 when none may be selected */
	unsigned int version;
};

static const struct poc_version_select_case poc_version_select_cases[] = {
	{ "version 1 selects layout 1", 0x00000001, 1 },
	{ "version 1 with NumberModule set selects layout 1", 0x00030001, 1 },
	{ "version 1 with all upper bits set selects layout 1", 0xffffff01, 1 },
	{ "version 0 is unknown", 0x00000000, 0 },
	{ "version 2 is unknown", 0x00000002, 0 },
	{ "version 255 is unknown", 0x000000ff, 0 },
	{ "version 0 with NumberModule set is unknown", 0x00000100, 0 },
};

static void poc_version_select_case_desc(const struct poc_version_select_case *c,
					 char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(poc_version_select, poc_version_select_cases,
		  poc_version_select_case_desc);

static void poc_version_select_test(struct kunit *test)
{
	const struct poc_version_select_case *c = test->param_value;
	const struct poc_version_layout *layout;

	layout = poc_version_layout_select(c->raw);
	if (!c->version) {
		KUNIT_EXPECT_PTR_EQ(test, layout,
				    (const struct poc_version_layout *)NULL);
		return;
	}

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, layout);
	KUNIT_EXPECT_EQ(test, layout->version, c->version);
}

/*
 * Value file visibility
 */

/*
 * Synthetic Register layout, never selected by probe: layout 1's blocks
 * without the Values common/vivado_version, top/git_url and uid/user_efuse.
 */
static const struct poc_version_value poc_version_test_partial_values[] = {
	POC_VERSION_VALUE(COMMON, "build_date", poc_version_v1_blocks,
			  POC_VERSION_V1_COMMON_BUILD_DATE,
			  poc_version_format_date),
	POC_VERSION_VALUE(COMMON, "version_of_version_reg",
			  poc_version_v1_blocks,
			  POC_VERSION_V1_COMMON_NUM_MOD_VERSION,
			  poc_version_format_version_of_version_reg),
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
	POC_VERSION_VALUE(UID, "uid", poc_version_v1_blocks,
			  POC_VERSION_V1_UID_UID, poc_version_format_hex),
	POC_VERSION_VALUE(UID, "user_id", poc_version_v1_blocks,
			  POC_VERSION_V1_UID_USER_ID, poc_version_format_hex),
};

static const struct poc_version_layout poc_version_test_partial_layout = {
	.version = 0xfe,
	.blocks = poc_version_v1_blocks,
	.nblocks = ARRAY_SIZE(poc_version_v1_blocks),
	.values = poc_version_test_partial_values,
	.nvalues = ARRAY_SIZE(poc_version_test_partial_values),
};

/* Every Value file of the sysfs ABI, as "<Category directory>/<file>" */
static const char *const poc_version_test_all_files[] = {
	"common/build_date",
	"common/version_of_version_reg",
	"common/vivado_version",
	"common/project_name",
	"top/version",
	"top/git_hash",
	"top/git_date",
	"top/git_time",
	"top/branch_name_tag",
	"top/git_url",
	"uid/uid",
	"uid/user_efuse",
	"uid/user_id",
};

#define POC_VERSION_TEST_MAX_FILES ARRAY_SIZE(poc_version_test_all_files)

struct poc_version_visible_case {
	const char *desc;
	const struct poc_version_layout *layout;
	/* Value files that must be absent; every other one must be present */
	const char *hidden[POC_VERSION_TEST_MAX_FILES];
};

static const struct poc_version_visible_case poc_version_visible_cases[] = {
	{
		.desc = "layout 1 exposes every Value file",
		.layout = &poc_version_layout_v1,
	},
	{
		.desc = "a layout lacking Values hides exactly those",
		.layout = &poc_version_test_partial_layout,
		.hidden = {
			"common/vivado_version",
			"top/git_url",
			"uid/user_efuse",
		},
	},
	{
		.desc = "an Instance without a layout exposes no Value file",
		.layout = NULL,
		.hidden = {
			"common/build_date",
			"common/version_of_version_reg",
			"common/vivado_version",
			"common/project_name",
			"top/version",
			"top/git_hash",
			"top/git_date",
			"top/git_time",
			"top/branch_name_tag",
			"top/git_url",
			"uid/uid",
			"uid/user_efuse",
			"uid/user_id",
		},
	},
};

static void poc_version_visible_case_desc(const struct poc_version_visible_case *c,
					  char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(poc_version_visible, poc_version_visible_cases,
		  poc_version_visible_case_desc);

static bool poc_version_test_is_hidden(const struct poc_version_visible_case *c,
				       const char *file)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c->hidden) && c->hidden[i]; i++)
		if (!strcmp(c->hidden[i], file))
			return true;

	return false;
}

/*
 * Walks the attribute groups the driver registers, i.e. the sysfs ABI, and
 * asks each group's is_visible callback, as the driver core does, about each
 * Value file of an Instance with the case's layout. Checks the result against
 * the expected set. Hidden Values must also not render through the seam.
 */
static void poc_version_visible_test(struct kunit *test)
{
	const struct poc_version_visible_case *c = test->param_value;
	struct poc_version_dev priv = { .layout = c->layout };
	unsigned int g, a, seen = 0;
	u32 regs[POC_VERSION_TEST_NREGS] = { };
	struct device dev = {};
	char file[64];
	char *buf;

	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* The Instance, as the driver core sees it after probe */
	dev_set_drvdata(&dev, &priv);

	for (g = 0; poc_version_groups[g]; g++) {
		const struct attribute_group *group = poc_version_groups[g];

		KUNIT_ASSERT_TRUE(test, group->is_visible);

		for (a = 0; group->attrs[a]; a++) {
			struct attribute *attr = group->attrs[a];
			struct poc_version_value_attr *value_attr =
				container_of(attr, struct poc_version_value_attr,
					     dev_attr.attr);
			bool hidden;
			umode_t mode;
			ssize_t len;

			snprintf(file, sizeof(file), "%s/%s", group->name,
				 attr->name);
			hidden = poc_version_test_is_hidden(c, file);
			mode = group->is_visible(&dev.kobj, attr, a);
			seen++;

			if (hidden) {
				KUNIT_EXPECT_EQ_MSG(test, mode, (umode_t)0,
						    "%s visible", file);
				if (c->layout) {
					len = poc_version_render(c->layout, regs,
								 ARRAY_SIZE(regs),
								 value_attr->category,
								 attr->name, buf);
					KUNIT_EXPECT_EQ_MSG(test, len,
							    (ssize_t)-ENOENT,
							    "%s renders", file);
				}
			} else {
				/* Present, and read-only (docs/adr/0003) */
				KUNIT_EXPECT_EQ_MSG(test, mode, (umode_t)0444,
						    "%s not visible", file);
				len = poc_version_render(c->layout, regs,
							 ARRAY_SIZE(regs),
							 value_attr->category,
							 attr->name, buf);
				KUNIT_EXPECT_GT_MSG(test, len, (ssize_t)0,
						    "%s does not render", file);
			}
		}
	}

	/* The groups hold exactly the Value files of the ABI */
	KUNIT_EXPECT_EQ(test, seen, (unsigned int)POC_VERSION_TEST_MAX_FILES);
	for (a = 0; a < ARRAY_SIZE(poc_version_test_all_files); a++) {
		const char *path = poc_version_test_all_files[a];
		const char *slash = strchr(path, '/');
		bool found = false;
		unsigned int i;

		for (g = 0; poc_version_groups[g]; g++) {
			const struct attribute_group *group = poc_version_groups[g];

			if (strncmp(group->name, path, slash - path) ||
			    group->name[slash - path])
				continue;
			for (i = 0; group->attrs[i]; i++)
				if (!strcmp(group->attrs[i]->name, slash + 1))
					found = true;
		}
		KUNIT_EXPECT_TRUE_MSG(test, found, "%s missing", path);
	}
}

#ifdef POC_VERSION_INJECTION
static const bool poc_version_test_injection = true;
#else
static const bool poc_version_test_injection;
#endif

/*
 * The writeable_reg callback of the Cached copy's regmap: without Injection
 * no offset is writeable; with it, exactly the Registers of layout 1 (0x000
 * to 0x11C) are.
 */
static void poc_version_writeable_test(struct kunit *test)
{
	bool (*writeable)(struct device *dev, unsigned int reg) =
		poc_version_writeable_reg;
	struct poc_version_dev priv = { };
	struct device dev = {};
	unsigned int reg;

	/* No drvdata, as before probe has finished */
	KUNIT_EXPECT_FALSE(test, writeable(&dev, 0x000));

	/* An Instance without a layout */
	dev_set_drvdata(&dev, &priv);
	KUNIT_EXPECT_FALSE(test, writeable(&dev, 0x000));

	priv.layout = &poc_version_layout_v1;
	for (reg = 0; reg < 2 * POC_VERSION_TEST_NREGS * 4; reg++) {
		bool expected = poc_version_test_injection &&
				IS_ALIGNED(reg, 4) &&
				reg < POC_VERSION_TEST_NREGS * 4;

		KUNIT_EXPECT_EQ_MSG(test, writeable(&dev, reg), expected,
				    "Register 0x%03x", reg);
	}
}

/*
 * Cached copy, through regmap's cache
 */

/* The Instance's Registers in memory, standing in for its MMIO */
struct poc_version_test_hw {
	u32 regs[POC_VERSION_TEST_NREGS];
	/* Register reads that reached the memory */
	unsigned int reads;
};

static int poc_version_test_hw_read(void *context, unsigned int reg,
				    unsigned int *val)
{
	struct poc_version_test_hw *hw = context;

	if (reg / 4 >= ARRAY_SIZE(hw->regs))
		return -EINVAL;

	*val = hw->regs[reg / 4];
	hw->reads++;

	return 0;
}

/* The driver's Cached copy configuration, over @hw instead of MMIO */
static struct regmap *
poc_version_test_init_cached_copy(struct poc_version_dev *priv,
				  struct poc_version_test_hw *hw)
{
	const struct regmap_config config =
		POC_VERSION_CACHED_COPY_CONFIG(priv->layout,
					       .reg_read = poc_version_test_hw_read);

	return devm_regmap_init(priv->dev, NULL, hw, &config);
}

/* top/version as sysfs shows it, through the driver's show callback */
static void poc_version_test_expect_version(struct kunit *test,
					    struct poc_version_dev *priv,
					    char *buf, const char *expected)
{
	ssize_t len;

	memset(buf, 0, PAGE_SIZE);
	len = poc_version_value_show(priv->dev,
				     &poc_version_attr_top_version.dev_attr,
				     buf);
	KUNIT_EXPECT_EQ(test, len, (ssize_t)strlen(expected));
	KUNIT_EXPECT_STREQ(test, buf, expected);
}

/*
 * An Instance whose Cached copy is a regmap with the driver's configuration,
 * and so its cache type (rbtree before 6.4, maple from 6.4), over memory
 * instead of MMIO. Probe's fill, a Value read, a change in the hardware
 * that the Cached copy must hide, and a Reread that must show it.
 */
static void poc_version_cached_copy_test(struct kunit *test)
{
	struct poc_version_test_hw *hw;
	struct poc_version_dev *priv;
	struct device *dev;
	unsigned int reads;
	char *buf;
	int ret;

	kunit_info(test, "regcache: %s\n",
		   POC_VERSION_KERNEL_AT_LEAST(6, 4) ? "maple" : "rbtree");

	hw = kunit_kzalloc(test, sizeof(*hw), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hw);
	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* Register layout 1, top/version 1.4.0 */
	hw->regs[POC_VERSION_TEST_VOVR_OFFSET / 4] = 1;
	hw->regs[0x020 / 4] = 0x01040000;

	priv->layout = poc_version_layout_select(hw->regs[POC_VERSION_TEST_VOVR_OFFSET / 4]);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->layout);

	/* Unregistering it releases its devres, the regmap included */
	dev = root_device_register("poc-version-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	priv->dev = dev;
	mutex_init(&priv->lock);
	dev_set_drvdata(dev, priv);

	priv->map = poc_version_test_init_cached_copy(priv, hw);
	if (IS_ERR(priv->map)) {
		KUNIT_FAIL(test, "regmap init failed: %ld", PTR_ERR(priv->map));
		goto out;
	}

	/* Probe's fill reads every Register of the layout once */
	ret = poc_version_reread_locked(priv);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, hw->reads, (unsigned int)POC_VERSION_TEST_NREGS);
	poc_version_test_expect_version(test, priv, buf, "1.4.0\n");

	/* The hardware changes; the Value still comes from the Cached copy */
	hw->regs[0x020 / 4] = 0x0104000c;
	reads = hw->reads;
	poc_version_test_expect_version(test, priv, buf, "1.4.0\n");
	KUNIT_EXPECT_EQ(test, hw->reads, reads);

	/* Reread, as the debugfs trigger does it */
	mutex_lock(&priv->lock);
	ret = poc_version_reread_locked(priv);
	mutex_unlock(&priv->lock);
	KUNIT_EXPECT_EQ(test, ret, 0);
	poc_version_test_expect_version(test, priv, buf, "1.4.0+3\n");

out:
	root_device_unregister(dev);
	mutex_destroy(&priv->lock);
}

static struct kunit_case poc_version_test_cases[] = {
	KUNIT_CASE_PARAM(poc_version_render_test,
			 poc_version_render_gen_params),
	KUNIT_CASE_PARAM(poc_version_select_test,
			 poc_version_select_gen_params),
	KUNIT_CASE_PARAM(poc_version_visible_test,
			 poc_version_visible_gen_params),
	KUNIT_CASE(poc_version_writeable_test),
	KUNIT_CASE(poc_version_cached_copy_test),
	{}
};

static struct kunit_suite poc_version_test_suite = {
	.name = "poc-version",
	.test_cases = poc_version_test_cases,
};

kunit_test_suite(poc_version_test_suite);
