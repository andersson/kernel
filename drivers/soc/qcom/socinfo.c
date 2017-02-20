// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2009-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017-2018, Linaro Ltd.
 */

#include <linux/err.h>
#include <linux/export.h>
#include <linux/libfdt.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/string.h>
#include <linux/sys_soc.h>
#include <linux/types.h>

/*
 * SOC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.  For example:
 *   1.0 -> 0x00010000
 *   2.3 -> 0x00020003
 */
#define SOCINFO_MAJOR(ver) (((ver) & 0xffff0000) >> 16)
#define SOCINFO_MINOR(ver) ((ver) & 0x0000ffff)

#define SMEM_SOCINFO_BUILD_ID_LENGTH		32

/*
 * SMEM item ids, used to acquire handles to respective
 * SMEM region.
 */
#define SMEM_HW_SW_BUILD_ID		137

/* Socinfo SMEM item structure */
struct socinfo {
	__le32 fmt;
	__le32 id;
	__le32 ver;
	char build_id[SMEM_SOCINFO_BUILD_ID_LENGTH];
	/* Version 2 */
	__le32 raw_id;
	__le32 raw_ver;
	/* Version 3 */
	__le32 hw_plat;
	/* Version 4 */
	__le32 plat_ver;
	/* Version 5 */
	__le32 accessory_chip;
	/* Version 6 */
	__le32 hw_plat_subtype;
	/* Version 7 */
	__le32 pmic_model;
	__le32 pmic_die_rev;
	/* Version 8 */
	__le32 pmic_model_1;
	__le32 pmic_die_rev_1;
	__le32 pmic_model_2;
	__le32 pmic_die_rev_2;
	/* Version 9 */
	__le32 foundry_id;
	/* Version 10 */
	__le32 serial_num;
	/* Version 11 */
	__le32 num_pmics;
	__le32 pmic_array_offset;
	/* Version 12 */
	__le32 chip_family;
	__le32 raw_device_family;
	__le32 raw_device_num;
};

static const struct {
	unsigned int id;
	const char *name;
} soc_of_id[] = {
	{87, "MSM8960"},
	{109, "APQ8064"},
	{122, "MSM8660A"},
	{123, "MSM8260A"},
	{124, "APQ8060A"},
	{126, "MSM8974"},
	{130, "MPQ8064"},
	{138, "MSM8960AB"},
	{139, "APQ8060AB"},
	{140, "MSM8260AB"},
	{141, "MSM8660AB"},
	{178, "APQ8084"},
	{184, "APQ8074"},
	{185, "MSM8274"},
	{186, "MSM8674"},
	{194, "MSM8974PRO"},
	{206, "MSM8916"},
	{208, "APQ8074-AA"},
	{209, "APQ8074-AB"},
	{210, "APQ8074PRO"},
	{211, "MSM8274-AA"},
	{212, "MSM8274-AB"},
	{213, "MSM8274PRO"},
	{214, "MSM8674-AA"},
	{215, "MSM8674-AB"},
	{216, "MSM8674PRO"},
	{217, "MSM8974-AA"},
	{218, "MSM8974-AB"},
	{246, "MSM8996"},
	{247, "APQ8016"},
	{248, "MSM8216"},
	{249, "MSM8116"},
	{250, "MSM8616"},
	{291, "APQ8096"},
	{305, "MSM8996SG"},
	{310, "MSM8996AU"},
	{311, "APQ8096AU"},
	{312, "APQ8096SG"},
};

/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT 12

static const char *socinfo_machine(unsigned int id)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(soc_of_id); idx++) {
		if (soc_of_id[idx].id == id)
			return soc_of_id[idx].name;
	}

	return NULL;
}

static int qcom_socinfo_probe(struct platform_device *pdev)
{
	struct soc_device_attribute *attr;
	struct soc_device *soc_dev;
	struct socinfo *socinfo;
	size_t item_size;

	socinfo = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID,
				&item_size);
	if (IS_ERR(socinfo)) {
		dev_err(&pdev->dev, "Couldn't find socinfo\n");
		return -EINVAL;
	}

	if (SOCINFO_MAJOR(le32_to_cpu(socinfo->fmt) != 0) ||
	    SOCINFO_MINOR(le32_to_cpu(socinfo->fmt) < 0)  ||
	    le32_to_cpu(socinfo->fmt) > MAX_SOCINFO_FORMAT) {
		dev_err(&pdev->dev, "Unsupported socinfo format\n");
		return -EINVAL;
	}

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	attr->family = "Snapdragon";
	attr->machine = socinfo_machine(le32_to_cpu(socinfo->id));
	attr->revision = kasprintf(GFP_KERNEL, "%u.%u",
				   SOCINFO_MAJOR(le32_to_cpu(socinfo->ver)),
				   SOCINFO_MINOR(le32_to_cpu(socinfo->ver)));
	if (le32_to_cpu(socinfo->fmt) >= 10)
		attr->serial_number = kasprintf(GFP_KERNEL, "%u",
					        le32_to_cpu(socinfo->serial_num));

	soc_dev = soc_device_register(attr);
	if (IS_ERR(soc_dev)) {
		kfree(attr);
		return PTR_ERR(soc_dev);
	}

	/* Feed the soc specific unique data into entropy pool */
	add_device_randomness(socinfo, item_size);

	platform_set_drvdata(pdev, soc_dev);

	return 0;
}

static int qcom_socinfo_remove(struct platform_device *pdev)
{
	struct soc_device *soc_dev = platform_get_drvdata(pdev);;

	soc_device_unregister(soc_dev);

	return 0;
}

static struct platform_driver qcom_socinfo_driver = {
	.probe = qcom_socinfo_probe,
	.remove = qcom_socinfo_remove,
	.driver  = {
		.name = "qcom-socinfo",
	},
};

module_platform_driver(qcom_socinfo_driver);

MODULE_DESCRIPTION("Qualcomm socinfo driver");
MODULE_LICENSE("GPL v2");
