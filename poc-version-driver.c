/*  poc-version-driver.c - The simplest kernel module.
*/

#include <linux/device.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/limits.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/string.h>
#include <linux/uuid.h>
#include <linux/stddef.h>
#include <linux/regmap.h>

/* 
 * Standard module information
 */
#define VERSION "1.0.0"
#define DRIVER_NAME "PoC-version"


/* 
 * Helper macros
 */
#define DEBUGFS_REG32_ENTRY(_struct, _member, _name) \
    { .name = (_name), .offset = offsetof(_struct, _member) }

#define DEBUGFS_REG32_ARRAY_ENTRY(_struct, _array, _idx, _name) \
    { .name = (_name), .offset = offsetof(_struct, _array[_idx]) }

/*
 * Register definitions
 */

/*
 * Common.BuildDate
 */
#define POC_REG_VERSION_COMMON_BUILD_DATE_OFFSET  0x000
#define POC_REG_VERSION_COMMON_BUILD_DATE_DAY_MASK 0xFF000000
#define POC_REG_VERSION_COMMON_BUILD_DATE_MONTH_MASK 0xFF0000
#define POC_REG_VERSION_COMMON_BUILD_DATE_YEAR_MASK 0xFFFF

/*
 * Common.NumberModule_VersionOfVersionReg
 */
#define POC_REG_VERSION_COMMON_NUM_MOD_VERSION_OFFSET  0x004
#define POC_REG_VERSION_COMMON_NUM_MOD_VERSION_VERSION_MASK 0xFF

/*
 * Common.VivadoVersion
 */
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_OFFSET  0x008
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_MAJOR_MASK 0xFFFF0000
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_MINOR_MASK 0xFF00
#define POC_REG_VERSION_COMMON_VIVADO_VERSION_PATCH_MASK 0xFF

/*
 * Common.ProjectName
 */
#define POC_REG_VERSION_COMMON_PROJECT_NAME_OFFSET  0x00C
#define POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH 20

/*
 * Top.Version
 */
#define POC_REG_VERSION_TOP_VERSION_OFFSET  0x020
#define POC_REG_VERSION_TOP_VERSION_MAJOR_MASK 0xFF000000
#define POC_REG_VERSION_TOP_VERSION_MINOR_MASK 0xFF0000
#define POC_REG_VERSION_TOP_VERSION_PATCH_MASK 0xFF00
#define POC_REG_VERSION_TOP_VERSION_COMMITS_TO_TAG_MASK 0xFC
#define POC_REG_VERSION_TOP_VERSION_UNTRACKED_MASK 0x2
#define POC_REG_VERSION_TOP_VERSION_MODIFIED_MASK 0x1

/*
 * Top.GitHash
 */
#define POC_REG_VERSION_TOP_GIT_HASH_OFFSET  0x024
#define POC_REG_VERSION_TOP_GIT_HASH_LENGTH 20

/*
 * Top.GitDate
 */
#define POC_REG_VERSION_TOP_GIT_DATE_OFFSET  0x038
#define POC_REG_VERSION_TOP_GIT_DATE_DAY_MASK 0xFF000000
#define POC_REG_VERSION_TOP_GIT_DATE_MONTH_MASK 0xFF0000
#define POC_REG_VERSION_TOP_GIT_DATE_YEAR_MASK 0xFFFF

/*
 * Top.GitTime
 */
#define POC_REG_VERSION_TOP_GIT_TIME_OFFSET  0x03C
#define POC_REG_VERSION_TOP_GIT_TIME_HOUR_MASK 0xFF000000
#define POC_REG_VERSION_TOP_GIT_TIME_MIN_MASK 0xFF0000
#define POC_REG_VERSION_TOP_GIT_TIME_SEC_MASK 0xFF00
#define POC_REG_VERSION_TOP_GIT_TIME_TIME_ZONE_MASK 0xFF

/*
 * Top.BranchName_Tag
 */
#define POC_REG_VERSION_TOP_BRANCH_NAME_TAG_OFFSET  0x040
#define POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH 64

/*
 * Top.GitURL
 */
#define POC_REG_VERSION_TOP_GIT_URL_OFFSET  0x080
#define POC_REG_VERSION_TOP_GIT_URL_LENGTH 128

/*
 * UID.UID
 */
#define POC_REG_VERSION_UID_UID_OFFSET  0x100
#define POC_REG_VERSION_UID_UID_LENGTH 16

/*
 * UID.User_eFuse
 */
#define POC_REG_VERSION_UID_USER_EFUSE_OFFSET  0x110
#define POC_REG_VERSION_UID_USER_EFUSE_LENGTH 4

/*
 * UID.User_ID
 */
#define POC_REG_VERSION_UID_USER_ID_OFFSET  0x114
#define POC_REG_VERSION_UID_USER_ID_LENGTH 12

struct PoC_version_reg {
    u32 common_build_date;
    u32 common_num_module_version;
    u32 common_vivado_version;
    u32 common_project_name[POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH / sizeof(u32)];
    u32 top_version;
    u32 top_git_hash[POC_REG_VERSION_TOP_GIT_HASH_LENGTH / sizeof(u32)];
    u32 top_git_date;
    u32 top_git_time;
    u32 top_git_branch_name_tag[POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH / sizeof(u32)];
    u32 top_git_url[POC_REG_VERSION_TOP_GIT_URL_LENGTH / sizeof(u32)];
    u32 uid_uid[POC_REG_VERSION_UID_UID_LENGTH / sizeof(u32)];
    u32 uid_user_efuse;
    u32 uid_user_id[POC_REG_VERSION_UID_USER_ID_LENGTH / sizeof(u32)];
};

struct PoC_version_dev;
struct PoC_debugfs_link {
	struct PoC_version_dev *priv;
    u32 *shadow;
};

struct PoC_version_dev {
	struct device *dev;
	void __iomem *regs;
	struct mutex lock;
	struct PoC_version_reg cached_regs;
	struct dentry *debugfs_dir;
	struct debugfs_regset32 regset;
	struct PoC_debugfs_link debugfs_links[sizeof(struct PoC_version_reg) / sizeof(u32)];
};

/*
 * Helper functions
 */
static inline u32 poc_version_read_reg(struct PoC_version_dev *dev, u32 offset)
{
	return ioread32(dev->regs + offset);
}

static void poc_version_cache_registers(struct PoC_version_dev *priv)
{
	memcpy_fromio(&priv->cached_regs, priv->regs, sizeof(struct PoC_version_reg));
}

/* 
 * Sysfs attributes
 */
static ssize_t reread_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct PoC_version_dev *priv = dev_get_drvdata(dev);
	unsigned long val = 0;
	int ret = 0;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	if (val == 1) {
		ret = mutex_lock_interruptible(&priv->lock);
		if (ret) {
			return ret;
		}

		poc_version_cache_registers(priv);

		mutex_unlock(&priv->lock);

		dev_info(dev, "registers re-read\n");
	}

	return count;
}


static DEVICE_ATTR_WO(reread);

static struct attribute *poc_version_attrs[] = {
	&dev_attr_reread.attr,
	NULL /* sentinel */,
};

static const struct attribute_group poc_version_attr_group = {
	.attrs = poc_version_attrs,
};

/* 
 * DebugFS interface
 */

 static const struct debugfs_reg32 poc_version_debugfs_regs[] = {
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, common_build_date, "Common.BuildDate"),
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, common_num_module_version, "Common.NumberModule_VersionOfVersionReg"),
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, common_vivado_version, "Common.VivadoVersion"),
    DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, common_project_name, 0, "Common.ProjectName0"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, common_project_name, 1, "Common.ProjectName1"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, common_project_name, 2, "Common.ProjectName2"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, common_project_name, 3, "Common.ProjectName3"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, common_project_name, 4, "Common.ProjectName4"),
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, top_version, "Top.Version"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_hash, 0, "Top.GitHash0"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_hash, 1, "Top.GitHash1"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_hash, 2, "Top.GitHash2"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_hash, 3, "Top.GitHash3"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_hash, 4, "Top.GitHash4"),
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, top_git_date, "Top.GitDate"),
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, top_git_time, "Top.GitTime"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 0, "Top.BranchName_Tag0"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 1, "Top.BranchName_Tag1"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 2, "Top.BranchName_Tag2"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 3, "Top.BranchName_Tag3"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 4, "Top.BranchName_Tag4"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 5, "Top.BranchName_Tag5"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 6, "Top.BranchName_Tag6"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 7, "Top.BranchName_Tag7"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 8, "Top.BranchName_Tag8"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 9, "Top.BranchName_Tag9"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 10, "Top.BranchName_Tag10"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 11, "Top.BranchName_Tag11"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 12, "Top.BranchName_Tag12"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 13, "Top.BranchName_Tag13"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 14, "Top.BranchName_Tag14"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_branch_name_tag, 15, "Top.BranchName_Tag15"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 0, "Top.GitURL0"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 1, "Top.GitURL1"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 2, "Top.GitURL2"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 3, "Top.GitURL3"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 4, "Top.GitURL4"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 5, "Top.GitURL5"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 6, "Top.GitURL6"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 7, "Top.GitURL7"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 8, "Top.GitURL8"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 9, "Top.GitURL9"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 10, "Top.GitURL10"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 11, "Top.GitURL11"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 12, "Top.GitURL12"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 13, "Top.GitURL13"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 14, "Top.GitURL14"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 15, "Top.GitURL15"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 16, "Top.GitURL16"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 17, "Top.GitURL17"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 18, "Top.GitURL18"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 19, "Top.GitURL19"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 20, "Top.GitURL20"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 21, "Top.GitURL21"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 22, "Top.GitURL22"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 23, "Top.GitURL23"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 24, "Top.GitURL24"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 25, "Top.GitURL25"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 26, "Top.GitURL26"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 27, "Top.GitURL27"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 28, "Top.GitURL28"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 29, "Top.GitURL29"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 30, "Top.GitURL30"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, top_git_url, 21, "Top.GitURL21"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_uid, 0, "UID.UID0"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_uid, 1, "UID.UID1"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_uid, 2, "UID.UID2"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_uid, 3, "UID.UID3"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_uid, 4, "UID.UID4"),
	DEBUGFS_REG32_ENTRY(struct PoC_version_reg, uid_user_efuse, "UID.User_eFuse"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_user_id, 0, "UID.User_ID0"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_user_id, 1, "UID.User_ID1"),
	DEBUGFS_REG32_ARRAY_ENTRY(struct PoC_version_reg, uid_user_id, 2, "UID.User_ID2"),
};

static int poc_version_debugfs_cached_reg32_get(void *data, u64 *val)
{
	int ret = 0;
    struct PoC_debugfs_link *link = data;
	struct PoC_version_dev *priv = link->priv;

    ret = mutex_lock_interruptible(&priv->lock);
	if (ret) {
		return ret;
	}

    *val = *link->shadow;

    mutex_unlock(&priv->lock);

    return 0;
}

static int poc_version_debugfs_cached_reg32_set(void *data, u64 val)
{
    int ret = 0;
    struct PoC_debugfs_link *link = data;
	struct PoC_version_dev *priv = link->priv;

    if (val > U32_MAX) {
        return -ERANGE;
	}

    ret = mutex_lock_interruptible(&priv->lock);
	if (ret) {
		return ret;
	}

    *link->shadow = (u32)val;

    mutex_unlock(&priv->lock);

    return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(poc_version_debugfs_cached_reg32_fops, poc_version_debugfs_cached_reg32_get, poc_version_debugfs_cached_reg32_set, "0x%08llx\n");



static void poc_version_create_debugfs_interface(struct PoC_version_dev *priv)
{
	priv->debugfs_dir = debugfs_create_dir(dev_name(priv->dev), NULL);
	if (IS_ERR_OR_NULL(priv->debugfs_dir)) {
		dev_info(priv->dev, "DebugFS interface is not available\n");
		return;
	}

	priv->regset.regs = poc_version_debugfs_regs;
    priv->regset.nregs = ARRAY_SIZE(poc_version_debugfs_regs);
    priv->regset.base = priv->regs;
	priv->regset.dev = priv->dev;

    debugfs_create_regset32(
        "registers",
        S_IRUSR | S_IRGRP | S_IROTH,
        priv->debugfs_dir,
        &priv->regset
    );

	struct dentry * cached_dir = debugfs_create_dir("cached_registers", priv->debugfs_dir);
	for (size_t i = 0; i < ARRAY_SIZE(priv->debugfs_links); ++i) {
		size_t const offset = (i * sizeof(u32));
		char const *name = NULL;
		for (size_t j = 0; j < ARRAY_SIZE(poc_version_debugfs_regs); ++j) {
			if (poc_version_debugfs_regs[j].offset == offset) {
				name = poc_version_debugfs_regs[j].name;
				break;
			}
		}

		if (unlikely(!name)) {
			continue;
		}

		priv->debugfs_links[i].priv = priv;
		priv->debugfs_links[i].shadow = (u32 *)((void *)&priv->cached_regs + offset);

		debugfs_create_file_unsafe(name,
							S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,
							cached_dir,
							&priv->debugfs_links[i],
							&poc_version_debugfs_cached_reg32_fops);
	}
}

/* 
 * Platform Device Driver
 */

static int poc_version_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct PoC_version_dev *priv = NULL;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(dev, "failed to allocate memory\n");
		ret = -ENOMEM;
		goto POC_VERSION_PROBE_RETURN;
	}

	priv->dev = dev;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "failed to map registers\n");
		ret = PTR_ERR(priv->regs);
		goto POC_VERSION_PROBE_RETURN;
	}

	mutex_init(&priv->lock);

	platform_set_drvdata(pdev, priv);

	poc_version_cache_registers(priv);

	poc_version_create_debugfs_interface(priv);
	
	dev_info(dev, "Probe %s successful\n", dev_name(&pdev->dev));

POC_VERSION_PROBE_RETURN:
	return ret;
}

static void poc_version_remove(struct platform_device *pdev)
{
	struct PoC_version_dev *priv = platform_get_drvdata(pdev);

	debugfs_remove(priv->debugfs_dir);
	mutex_destroy(&priv->lock);
}


static struct of_device_id poc_version_of_match[] = {
	{ .compatible = "PoC,version-1.0", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, poc_version_of_match);


static struct platform_driver poc_version_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table	= poc_version_of_match,
		// .dev_groups = {0},
	},
	.probe		= poc_version_probe,
	.remove		= poc_version_remove,
};

module_platform_driver(poc_version_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Navid Jalali <navidcity@gmail.com>");
MODULE_DESCRIPTION("PoC-version - Driver for PoC version PL IP core");
MODULE_VERSION(VERSION);
