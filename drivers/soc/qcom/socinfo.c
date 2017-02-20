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
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT		32
#define SMEM_IMAGE_VERSION_SIZE			4096
#define SMEM_IMAGE_VERSION_NAME_SIZE		75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE		20
#define SMEM_IMAGE_VERSION_OEM_SIZE		32

/*
 * SMEM item ids, used to acquire handles to respective
 * SMEM region.
 */
#define SMEM_IMAGE_VERSION_TABLE	469
#define SMEM_HW_SW_BUILD_ID		137

/*
 * SMEM Image table indices
 */
#define SMEM_IMAGE_TABLE_BOOT_INDEX	0
#define SMEM_IMAGE_TABLE_TZ_INDEX	1
#define SMEM_IMAGE_TABLE_RPM_INDEX	3
#define SMEM_IMAGE_TABLE_APPS_INDEX	10
#define SMEM_IMAGE_TABLE_MPSS_INDEX	11
#define SMEM_IMAGE_TABLE_ADSP_INDEX	12
#define SMEM_IMAGE_TABLE_CNSS_INDEX	13
#define SMEM_IMAGE_TABLE_VIDEO_INDEX	14

struct qcom_socinfo_attr {
	struct device_attribute attr;
	int min_ver;
};

#define QCOM_SOCINFO_ATTR(_name, _show, _min_ver) \
	{ __ATTR(_name, 0444, _show, NULL), _min_ver }


struct smem_image_attribute {
	struct device_attribute version;
	struct device_attribute variant;
	struct device_attribute crm;
	int index;
};

#define QCOM_SMEM_IMG_ITEM(_name, _mode, _index) \
	static struct smem_image_attribute _name##_image_attrs = { \
		__ATTR(_name##_image_version, _mode, \
			qcom_show_image_version, qcom_store_image_version), \
		__ATTR(_name##_image_variant, _mode, \
			qcom_show_image_variant, qcom_store_image_variant), \
		__ATTR(_name##_image_crm, _mode, \
			qcom_show_image_crm, qcom_store_image_crm), \
		_index \
	}; \
	static const struct attribute_group _name##_image_attr_group = { \
		.attrs = (struct attribute*[]) { \
			&_name##_image_attrs.version.attr, \
			&_name##_image_attrs.variant.attr, \
			&_name##_image_attrs.crm.attr, \
			NULL \
		} \
	}

static const char *const pmic_model[] = {
	[0]  = "Unknown PMIC model",
	[9]  = "PM8994",
	[11] = "PM8916",
	[13] = "PM8058",
	[14] = "PM8028",
	[15] = "PM8901",
	[16] = "PM8027",
	[17] = "ISL9519",
	[18] = "PM8921",
	[19] = "PM8018",
	[20] = "PM8015",
	[21] = "PM8014",
	[22] = "PM8821",
	[23] = "PM8038",
	[24] = "PM8922",
	[25] = "PM8917",
};

struct smem_image_version {
	char name[SMEM_IMAGE_VERSION_NAME_SIZE];
	char variant[SMEM_IMAGE_VERSION_VARIANT_SIZE];
	char pad;
	char oem[SMEM_IMAGE_VERSION_OEM_SIZE];
};

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
} *socinfo;

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

static struct smem_image_version *smem_image_version;

/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT 12

/* socinfo: sysfs functions */

static ssize_t
qcom_show_raw_version(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", le32_to_cpu(socinfo->raw_ver));
}

static ssize_t
qcom_show_build_id(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", socinfo->build_id);
}

static ssize_t
qcom_show_hw_platform(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", le32_to_cpu(socinfo->hw_plat));
}

static ssize_t
qcom_show_platform_version(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 le32_to_cpu(socinfo->plat_ver));
}

static ssize_t
qcom_show_accessory_chip(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 le32_to_cpu(socinfo->accessory_chip));
}

static ssize_t
qcom_show_platform_subtype(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	int subtype = le32_to_cpu(socinfo->hw_plat_subtype);

	if (subtype < 0)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%u\n", subtype);
}

static ssize_t
qcom_show_foundry_id(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 le32_to_cpu(socinfo->foundry_id));
}

static ssize_t
qcom_show_chip_family(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n",
			 le32_to_cpu(socinfo->chip_family));
}

static ssize_t
qcom_show_raw_device_family(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n",
			 le32_to_cpu(socinfo->raw_device_family));
}

static ssize_t
qcom_show_raw_device_number(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n",
			 le32_to_cpu(socinfo->raw_device_num));
}

static ssize_t
qcom_show_pmic_model(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	int model = SOCINFO_MINOR(le32_to_cpu(socinfo->pmic_model));

	if (model < 0)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%s\n", pmic_model[model]);
}

static ssize_t
qcom_show_pmic_die_revision(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u.%u\n",
			 SOCINFO_MAJOR(le32_to_cpu(socinfo->pmic_die_rev)),
			 SOCINFO_MINOR(le32_to_cpu(socinfo->pmic_die_rev)));
}

static ssize_t
qcom_show_image_version(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct smem_image_attribute *smem_attr;

	smem_attr = container_of(attr, struct smem_image_attribute, version);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 smem_image_version[smem_attr->index].name);
}

static ssize_t
qcom_store_image_version(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t size)
{
	struct smem_image_attribute *smem_attr;

	smem_attr = container_of(attr, struct smem_image_attribute, version);
	return strlcpy(smem_image_version[smem_attr->index].name, buf,
		       SMEM_IMAGE_VERSION_NAME_SIZE);
}

static ssize_t
qcom_show_image_variant(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct smem_image_attribute *smem_attr;

	smem_attr = container_of(attr, struct smem_image_attribute, variant);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 smem_image_version[smem_attr->index].variant);
}

static ssize_t
qcom_store_image_variant(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t size)
{
	struct smem_image_attribute *smem_attr;

	smem_attr = container_of(attr, struct smem_image_attribute, variant);
	return strlcpy(smem_image_version[smem_attr->index].variant, buf,
		       SMEM_IMAGE_VERSION_VARIANT_SIZE);
}

static ssize_t
qcom_show_image_crm(struct device *dev, struct device_attribute *attr,
		    char *buf)
{
	struct smem_image_attribute *smem_attr;

	smem_attr = container_of(attr, struct smem_image_attribute, crm);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 smem_image_version[smem_attr->index].oem);
}

static ssize_t
qcom_store_image_crm(struct device *dev, struct device_attribute *attr,
		     const char *buf, size_t size)
{
	struct smem_image_attribute *smem_attr;

	smem_attr = container_of(attr, struct smem_image_attribute, crm);
	return strlcpy(smem_image_version[smem_attr->index].oem, buf,
		       SMEM_IMAGE_VERSION_OEM_SIZE);
}

static const struct qcom_socinfo_attr socinfo_attrs[] = {
	QCOM_SOCINFO_ATTR(chip_family, qcom_show_chip_family, 12),
	QCOM_SOCINFO_ATTR(raw_device_family, qcom_show_raw_device_family, 12),
	QCOM_SOCINFO_ATTR(raw_device_number, qcom_show_raw_device_number, 12),
	QCOM_SOCINFO_ATTR(foundry_id, qcom_show_foundry_id, 9),
	QCOM_SOCINFO_ATTR(pmic_model, qcom_show_pmic_model, 7),
	QCOM_SOCINFO_ATTR(pmic_die_revision, qcom_show_pmic_die_revision, 7),
	QCOM_SOCINFO_ATTR(platform_subtype, qcom_show_platform_subtype, 6),
	QCOM_SOCINFO_ATTR(accessory_chip, qcom_show_accessory_chip, 5),
	QCOM_SOCINFO_ATTR(platform_version, qcom_show_platform_version, 4),
	QCOM_SOCINFO_ATTR(hw_platform, qcom_show_hw_platform, 3),
	QCOM_SOCINFO_ATTR(raw_version, qcom_show_raw_version, 2),
	QCOM_SOCINFO_ATTR(build_id, qcom_show_build_id, 1),
};

QCOM_SMEM_IMG_ITEM(boot, 0444, SMEM_IMAGE_TABLE_BOOT_INDEX);
QCOM_SMEM_IMG_ITEM(tz, 0444, SMEM_IMAGE_TABLE_TZ_INDEX);
QCOM_SMEM_IMG_ITEM(rpm, 0444, SMEM_IMAGE_TABLE_RPM_INDEX);
QCOM_SMEM_IMG_ITEM(apps, 0644, SMEM_IMAGE_TABLE_APPS_INDEX);
QCOM_SMEM_IMG_ITEM(mpss, 0444, SMEM_IMAGE_TABLE_MPSS_INDEX);
QCOM_SMEM_IMG_ITEM(adsp, 0444, SMEM_IMAGE_TABLE_ADSP_INDEX);
QCOM_SMEM_IMG_ITEM(cnss, 0444, SMEM_IMAGE_TABLE_CNSS_INDEX);
QCOM_SMEM_IMG_ITEM(video, 0444, SMEM_IMAGE_TABLE_VIDEO_INDEX);

static const
struct attribute_group *smem_img_tbl[SMEM_IMAGE_VERSION_BLOCKS_COUNT] = {
		[SMEM_IMAGE_TABLE_BOOT_INDEX] = &boot_image_attr_group,
		[SMEM_IMAGE_TABLE_TZ_INDEX] = &tz_image_attr_group,
		[SMEM_IMAGE_TABLE_RPM_INDEX] = &rpm_image_attr_group,
		[SMEM_IMAGE_TABLE_APPS_INDEX] = &apps_image_attr_group,
		[SMEM_IMAGE_TABLE_MPSS_INDEX] = &mpss_image_attr_group,
		[SMEM_IMAGE_TABLE_ADSP_INDEX] = &adsp_image_attr_group,
		[SMEM_IMAGE_TABLE_CNSS_INDEX] = &cnss_image_attr_group,
		[SMEM_IMAGE_TABLE_VIDEO_INDEX] = &video_image_attr_group,
};

static int socinfo_versions_init(struct device *device,
				 struct soc_device *soc_dev)
{
	struct device *dev;
	size_t size;
	int i;

	dev = soc_device_to_device(soc_dev);

	smem_image_version = qcom_smem_get(QCOM_SMEM_HOST_ANY,
					   SMEM_IMAGE_VERSION_TABLE,
					   &size);
	if (IS_ERR(smem_image_version) || (size != SMEM_IMAGE_VERSION_SIZE)) {
		dev_dbg(device, "Image version table absent\n");
		return 0;
	}

	for (i = 0; i < SMEM_IMAGE_VERSION_BLOCKS_COUNT; i++) {
		if (smem_img_tbl[i])
			WARN_ON(sysfs_create_group(&dev->kobj,
				smem_img_tbl[i]));
	}

	return 0;
}

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
	struct device *dev;
	size_t item_size;
	int i;

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

	socinfo_versions_init(&pdev->dev, soc_dev);

	dev = soc_device_to_device(soc_dev);
	for (i = 0; i < ARRAY_SIZE(socinfo_attrs); i++) {
		if (socinfo_attrs[i].min_ver <=	le32_to_cpu(socinfo->fmt))
			device_create_file(dev, &socinfo_attrs[i].attr);
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
