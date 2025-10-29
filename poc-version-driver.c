/*  poc-version-driver.c - The simplest kernel module.
*/

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>


/* Standard module information, edit as appropriate */
#define VERSION "1.0.0"
#define DRIVER_NAME "PoC-version"

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int poc_version_probe(struct platform_device *pdev)
{
	return 0;
}

static int poc_version_remove(struct platform_device *pdev)
{
	return 0;
}


static struct of_device_id poc_version_of_match[] = {
	{ .compatible = "PoC,version-1.0", },
	{ /* end of list */ },
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
