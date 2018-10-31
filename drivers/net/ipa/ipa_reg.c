// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/io.h>

#include "ipa.h"
#include "ipa_reg.h"

int ipa_reg_init(struct ipa *ipa)
{
	struct resource *res;

	/* Setup IPA register memory  */
	res = platform_get_resource_byname(ipa->pdev, IORESOURCE_MEM,
					   "ipa-reg");
	if (!res)
		return -ENODEV;

	ipa->reg_virt = ioremap(res->start, resource_size(res));
	if (!ipa->reg_virt)
		return -ENOMEM;
	ipa->reg_addr = res->start;

	return 0;
}

void ipa_reg_exit(struct ipa *ipa)
{
	iounmap(ipa->reg_virt);
}
