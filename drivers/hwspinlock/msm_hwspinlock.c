/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>

#include "hwspinlock_internal.h"

#define SPINLOCK_ID_APPS_PROC	1
#define BASE_ID			0

static int msm_hwspinlock_trylock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	writel_relaxed(SPINLOCK_ID_APPS_PROC, lock_addr);

	return readl_relaxed(lock_addr) == SPINLOCK_ID_APPS_PROC;
}

static void msm_hwspinlock_unlock(struct hwspinlock *lock)
{
	u32 lock_owner;
	void __iomem *lock_addr = lock->priv;

	lock_owner = readl_relaxed(lock_addr);
	if (lock_owner != SPINLOCK_ID_APPS_PROC) {
		pr_err("%s: spinlock not owned by us (actual owner is %d)\n",
				__func__, lock_owner);
	}

	writel_relaxed(0, lock_addr);
}

static const struct hwspinlock_ops msm_hwspinlock_ops = {
	.trylock	= msm_hwspinlock_trylock,
	.unlock		= msm_hwspinlock_unlock,
};

static const struct of_device_id msm_hwspinlock_of_match[] = {
	{
		.compatible = "qcom,tcsr-mutex",
		.data = (void *)0x80, /* register stride */
	},
	{ },
};

static int msm_hwspinlock_probe(struct platform_device *pdev)
{
	int ret, i, stride;
	size_t array_size;
	u32 num_locks;
	struct hwspinlock_device *bank;
	struct hwspinlock *hwlock;
	struct resource *res;
	void __iomem *iobase;
	struct device_node *node = pdev->dev.of_node;
	const struct of_device_id *match;

	match = of_match_device(msm_hwspinlock_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	ret = of_property_read_u32(node, "qcom,num-locks", &num_locks);
	if (ret || num_locks == 0)
		return -ENODEV;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mutex-base");
	iobase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(iobase))
		return PTR_ERR(iobase);

	array_size = num_locks * sizeof(*hwlock);
	bank = devm_kzalloc(&pdev->dev, sizeof(*bank) + array_size, GFP_KERNEL);
	if (!bank)
		return -ENOMEM;

	platform_set_drvdata(pdev, bank);

	stride = (int)match->data;
	for (i = 0, hwlock = &bank->lock[0]; i < num_locks; i++, hwlock++)
		hwlock->priv = iobase + i * stride;

	pm_runtime_enable(&pdev->dev);

	ret = hwspin_lock_register(bank, &pdev->dev, &msm_hwspinlock_ops,
						BASE_ID, num_locks);
	if (ret)
		pm_runtime_disable(&pdev->dev);

	return ret;
}

static int msm_hwspinlock_remove(struct platform_device *pdev)
{
	struct hwspinlock_device *bank = platform_get_drvdata(pdev);
	int ret;

	ret = hwspin_lock_unregister(bank);
	if (ret) {
		dev_err(&pdev->dev, "%s failed: %d\n", __func__, ret);
		return ret;
	}

	pm_runtime_disable(&pdev->dev);

	return 0;
}

static struct platform_driver msm_hwspinlock_driver = {
	.probe		= msm_hwspinlock_probe,
	.remove		= msm_hwspinlock_remove,
	.driver		= {
		.name	= "msm_hwspinlock",
		.owner	= THIS_MODULE,
		.of_match_table = msm_hwspinlock_of_match,
	},
};

static int __init msm_hwspinlock_init(void)
{
	return platform_driver_register(&msm_hwspinlock_driver);
}
/* board init code might need to reserve hwspinlocks for predefined purposes */
postcore_initcall(msm_hwspinlock_init);

static void __exit msm_hwspinlock_exit(void)
{
	platform_driver_unregister(&msm_hwspinlock_driver);
}
module_exit(msm_hwspinlock_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hardware spinlock driver for MSM");
MODULE_AUTHOR("Kumar Gala <galak@codeaurora.org>");
MODULE_AUTHOR("Jeffrey Hugo <jhugo@codeaurora.org>");
MODULE_AUTHOR("Eric Holmberg <eholmber@codeaurora.org>");
