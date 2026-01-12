/*  poc-version-driver.c - The simplest kernel module.
*/

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/printk.h>

/* 
 * Standard module information
 */
#define VERSION "1.0.0"
#define DRIVER_NAME "PoC-version"

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
	u8 common_project_name[POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH];
	u32 top_version;
	u8 top_git_hash[POC_REG_VERSION_TOP_GIT_HASH_LENGTH];
	u32 top_git_date;
	u32 top_git_time;
	u8 top_git_branch_name_tag[POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH];
	u8 top_git_url[POC_REG_VERSION_TOP_GIT_URL_LENGTH];
	u8 uid_uid[POC_REG_VERSION_UID_UID_LENGTH];
	u8 uid_user_efuse[POC_REG_VERSION_UID_USER_EFUSE_LENGTH];
	u8 uid_user_id[POC_REG_VERSION_UID_USER_ID_LENGTH];
};

struct PoC_version_dev {
	struct device *dev;
	struct miscdevice miscdev;
	struct dentry *debugfs_dir;
	void __iomem *regs;
	struct PoC_version_reg cached_regs;
	struct mutex lock;
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
	priv->cached_regs.common_build_date = poc_version_read_reg(priv, POC_REG_VERSION_COMMON_BUILD_DATE_OFFSET);
	priv->cached_regs.common_num_module_version = poc_version_read_reg(priv, POC_REG_VERSION_COMMON_NUM_MOD_VERSION_OFFSET);
	priv->cached_regs.common_vivado_version = poc_version_read_reg(priv, POC_REG_VERSION_COMMON_VIVADO_VERSION_OFFSET);
	memcpy_fromio(&priv->cached_regs.common_project_name, priv->regs + POC_REG_VERSION_COMMON_PROJECT_NAME_OFFSET, POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH);
	priv->cached_regs.top_version = poc_version_read_reg(priv, POC_REG_VERSION_TOP_VERSION_OFFSET);
	memcpy_fromio(&priv->cached_regs.top_git_hash, priv->regs + POC_REG_VERSION_TOP_GIT_HASH_OFFSET, POC_REG_VERSION_TOP_GIT_HASH_LENGTH);
	priv->cached_regs.top_git_date = poc_version_read_reg(priv, POC_REG_VERSION_TOP_GIT_DATE_OFFSET);
	priv->cached_regs.top_git_time = poc_version_read_reg(priv, POC_REG_VERSION_TOP_GIT_TIME_OFFSET);
	memcpy_fromio(&priv->cached_regs.top_git_branch_name_tag, priv->regs + POC_REG_VERSION_TOP_BRANCH_NAME_TAG_OFFSET, POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH);
	memcpy_fromio(&priv->cached_regs.top_git_url, priv->regs + POC_REG_VERSION_TOP_GIT_URL_OFFSET, POC_REG_VERSION_TOP_GIT_URL_LENGTH);
	memcpy_fromio(&priv->cached_regs.uid_uid, priv->regs + POC_REG_VERSION_UID_UID_OFFSET, POC_REG_VERSION_UID_UID_LENGTH);
	memcpy_fromio(&priv->cached_regs.uid_user_efuse, priv->regs + POC_REG_VERSION_UID_USER_EFUSE_OFFSET, POC_REG_VERSION_UID_USER_EFUSE_LENGTH);
	memcpy_fromio(&priv->cached_regs.uid_user_id, priv->regs + POC_REG_VERSION_UID_USER_ID_OFFSET, POC_REG_VERSION_UID_USER_ID_LENGTH);
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
		mutex_lock(&priv->lock);
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
static int poc_version_debugfs_registers_show(struct seq_file *s, void *unused)
{
	struct PoC_version_dev *priv = s->private;
	struct PoC_version_reg cached_regs = 0;

	mutex_lock(&priv->lock);
	cached_regs = priv->cached_regs;
	mutex_unlock(&priv->lock);

	seq_printf(s, "Common.BuildDate:  0x%08x\n", cached_regs.common_build_date);
	seq_printf(s, "Common.NumberModule_VersionOfVersionReg:  0x%08x\n", cached_regs.common_num_module_version);
	seq_printf(s, "Common.VivadoVersion:  0x%08x\n", cached_regs.common_vivado_version);
	seq_printf(s, "Common.ProjectName: %*ph\n", POC_REG_VERSION_COMMON_PROJECT_NAME_LENGTH, cached_regs.common_project_name);
	seq_printf(s, "Top.Version:  0x%08x\n", cached_regs.top_version);
	seq_printf(s, "Top.GitHash: %*ph\n", POC_REG_VERSION_TOP_GIT_HASH_LENGTH, cached_regs.top_git_hash);
	seq_printf(s, "Top.GitDate:  0x%08x\n", cached_regs.top_git_date);
	seq_printf(s, "Top.GitTime:  0x%08x\n", cached_regs.top_git_time);
	seq_printf(s, "Top.BranchName_Tag: %*ph\n", POC_REG_VERSION_TOP_BRANCH_NAME_TAG_LENGTH, cached_regs.top_git_branch_name_tag);
	seq_printf(s, "Top.GitURL: %*ph\n", POC_REG_VERSION_TOP_GIT_URL_LENGTH, cached_regs.top_git_url);
	seq_printf(s, "UID.UID: %*ph\n", POC_REG_VERSION_UID_UID_LENGTH, cached_regs.uid_uid);
	seq_printf(s, "UID.User_eFuse: %*ph\n", POC_REG_VERSION_UID_USER_EFUSE_LENGTH, cached_regs.uid_user_efuse);
	seq_printf(s, "UID.User_ID: %*ph\n", POC_REG_VERSION_UID_USER_ID_LENGTH, cached_regs.uid_user_id);

	return 0;
}

static int poc_version_debugfs_registers_open(struct inode *inode, struct file *file)
{
	return single_open(file, poc_version_debugfs_show, inode->i_private);
}

static const struct file_operations poc_version_debugfs_registers_fops = {
	.owner = THIS_MODULE,
	.open = poc_version_debugfs_open,
	.read = seq_read,
	.write   = poc_version_debugfs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t poc_version_debugfs_overwrite_write(struct file *file,
				   const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct PoC_version_dev *priv = file->private_data;
	char buf[64];
	u32 version = 0;
	int ret = 0;

	if (count >= sizeof(buf)) {
		return -EINVAL;
	}

	if (copy_from_user(buf, ubuf, count)) {
		return -EFAULT;
	}

	buf[count] = '\0';

	/* Accept hex values, e.g. "version=0x1234" */
	ret = sscanf(buf, "version=%x", &version);
	if (ret != 1) {
		return -EINVAL;
	}

	mutex_lock(&priv->lock);
	priv->cached_regs.common_num_module_version = version;
	mutex_unlock(&priv->lock);

	dev_info(priv->dev, "debugfs: cache overwritten\n");

	return count;
}

static const struct file_operations poc_version_debugfs_overwrite_fops = {
	.write   = poc_version_debugfs_overwrite_write,
	.open    = simple_open,
	.llseek  = no_llseek,
};

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
		return -ENOMEM;
	}

	priv->dev = dev;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "failed to map registers\n");
		return PTR_ERR(priv->regs);
	}

	mutex_init(&priv->lock);

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name  = dev_name(&pdev->dev);
	ret = misc_register(&priv->miscdev);
	if (res) {
		dev_err(dev, "failed (%d) to register misc device\n", ret);
		goto poc_version_probe_error_2;
	}

	platform_set_drvdata(pdev, priv);

	poc_version_cache_registers(priv);

	ret = sysfs_create_group(&priv->miscdev.this_device->kobj,
				 &poc_version_attr_group);
	if (ret) {
		dev_err(dev, "failed (%d) to register sysfs interface\n", ret);
		goto poc_version_probe_error_1;
	}

	priv->debugfs_dir = debugfs_create_dir(dev_name(&pdev->dev), NULL);
	if (IS_ERR_OR_NULL(priv->debugfs_dir)) {
		dev_warn(dev, "failed to create DebugFS interface\n");
	} else {
		debugfs_create_file("registers", 0444, dir, priv, &poc_version_debugfs_registers_fops);
		debugfs_create_file("overwrite", 0200, dir, priv, &poc_version_debugfs_overwrite_fops);
	}

	dev_info(dev, "Probe %s successful\n", dev_name(&pdev->dev));

	return 0;

poc_version_probe_error_1:
	misc_deregister(&priv->miscdev);
poc_version_probe_error_2:
	mutex_destroy(&priv->lock);

	return ret;
}

static int poc_version_remove(struct platform_device *pdev)
{
	struct PoC_version_dev *priv = platform_get_drvdata(pdev);

	sysfs_remove_group(&priv->miscdev.this_device->kobj, &poc_version_attr_group);
	misc_deregister(&priv->miscdev);
	mutex_destroy(&priv->lock);

	return 0;
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
	},
	.probe		= poc_version_probe,
	.remove		= poc_version_remove,
};

module_platform_driver(poc_version_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR
    ("Navid Jalali <navidcity@gmail.com>");
MODULE_DESCRIPTION
    ("PoC-version - Driver for PoC version PL IP core");

MODULE_VERSION(VERSION);
