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

/* 
 * Standard module information
 */
#define VERSION "1.0.0"
#define DRIVER_NAME "PoC-version"

/*
 * Register list
 */
#define POC_REG_VERSION_OFFSET  0x00

struct PoC_version_dev {
	struct device *dev;
	struct miscdevice miscdev;
	struct dentry *debugfs_dir;
	void __iomem *regs;
	u32 cached_version;
	struct mutex lock;
};

/*
 * Helper functions
 */
static inline u32 poc_version_read_reg(struct PoC_version_dev *dev, u32 offset)
{
	return ioread32(dev->regs + offset);
}

static void poc_version_refresh(struct mydev_priv *priv)
{
	priv->cached_version  = poc_version_read_reg(priv, POC_REG_VERSION_OFFSET);
}

/* 
 * Sysfs attributes
 */
static ssize_t version_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
	struct PoC_version_dev *priv = dev_get_drvdata(dev);
	u32 cached_version = 0;

	mutex_lock(&priv->lock);
	cached_version = priv->cached_version;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "0x%08x\n", cached_version);
}

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
		poc_version_refresh(priv);
		mutex_unlock(&priv->lock);
		dev_info(dev, "registers re-read\n");
	}

	return count;
}


static DEVICE_ATTR_RO(version_show);
static DEVICE_ATTR_WO(reread);

static struct attribute *poc_version_attrs[] = {
	&dev_attr_version.attr,
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
	u32 cached_version = 0;

	mutex_lock(&priv->lock);
	cached_version = priv->cached_version;
	mutex_unlock(&priv->lock);

	seq_printf(s, "VERSION:  0x%08x\n", cached_version);

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
	priv->cached_version = version;
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

	poc_version_refresh(priv);

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
