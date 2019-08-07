// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019 Linaro Ltd.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>

struct pil_reloc_entry {
	char name[8];
	__le64 base;
	__le32 size;
} __packed;

#define PIL_INFO_SIZE	200
#define PIL_INFO_ENTRIES (PIL_INFO_SIZE / sizeof(struct pil_reloc_entry))

struct pil_reloc {
	struct device *dev;
	struct regmap *map;
	u32 offset;
	int val_bytes;

	struct pil_reloc_entry entries[PIL_INFO_ENTRIES];
};

static struct pil_reloc *_reloc;
static DEFINE_MUTEX(reloc_mutex);

/**
 * qcom_pil_info_store() - store PIL information of image in IMEM
 * @image:	name of the image
 * @base:	base address of the loaded image
 * @size:	size of the loaded image
 */
void qcom_pil_info_store(const char *image, phys_addr_t base, size_t size)
{
	struct pil_reloc_entry *entry;
	int idx = -1;
	int i;

	mutex_lock(&reloc_mutex);
	if (!_reloc)
		goto unlock;

	for (i = 0; i < PIL_INFO_ENTRIES; i++) {
		if (!_reloc->entries[i].name[0]) {
			if (idx == -1)
				idx = i;
			continue;
		}

		if (!strncmp(_reloc->entries[i].name, image, 8)) {
			idx = i;
			goto found;
		}
	}

	if (idx) {
		dev_warn(_reloc->dev, "insufficient PIL info slots\n");
		goto unlock;
	}

found:
	entry = &_reloc->entries[idx];
	strlcpy(entry->name, image, sizeof(entry->name));
	entry->base = base;
	entry->size = size;

	regmap_bulk_write(_reloc->map, _reloc->offset + idx * sizeof(*entry),
			  entry, sizeof(*entry) / _reloc->val_bytes);

unlock:
	mutex_unlock(&reloc_mutex);
}
EXPORT_SYMBOL_GPL(qcom_pil_info_store);

/**
 * qcom_pil_info_available() - query if the pil info is probed
 *
 * Return: boolean indicating if the pil info device is probed
 */
bool qcom_pil_info_available(void)
{
	return !!_reloc;
}
EXPORT_SYMBOL_GPL(qcom_pil_info_available);

static int pil_reloc_probe(struct platform_device *pdev)
{
	struct pil_reloc *reloc;

	reloc = devm_kzalloc(&pdev->dev, sizeof(*reloc), GFP_KERNEL);
	if (!reloc)
		return -ENOMEM;

	reloc->dev = &pdev->dev;
	reloc->map = syscon_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(reloc->map))
		return PTR_ERR(reloc->map);

	if (of_property_read_u32(pdev->dev.of_node, "offset", &reloc->offset))
		return -EINVAL;

	reloc->val_bytes = regmap_get_val_bytes(reloc->map);
	if (reloc->val_bytes < 0)
		return -EINVAL;

	regmap_bulk_write(reloc->map, reloc->offset, reloc->entries,
			  sizeof(reloc->entries) / reloc->val_bytes);

	mutex_lock(&reloc_mutex);
	_reloc = reloc;
	mutex_unlock(&reloc_mutex);

	return 0;
}

static int pil_reloc_remove(struct platform_device *pdev)
{
	mutex_lock(&reloc_mutex);
	_reloc = NULL;
	mutex_unlock(&reloc_mutex);

	return 0;
}

static const struct of_device_id pil_reloc_of_match[] = {
	{ .compatible = "qcom,pil-reloc-info" },
	{}
};
MODULE_DEVICE_TABLE(of, pil_reloc_of_match);

static struct platform_driver pil_reloc_driver = {
	.probe = pil_reloc_probe,
	.remove = pil_reloc_remove,
	.driver = {
		.name = "qcom-pil-reloc-info",
		.of_match_table = pil_reloc_of_match,
	},
};
module_platform_driver(pil_reloc_driver);

MODULE_DESCRIPTION("Qualcomm PIL relocation info");
MODULE_LICENSE("GPL v2");
