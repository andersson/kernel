/*
 * Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2016 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/remoteproc.h>
#include <linux/pm_runtime.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-ioctl.h>

#include "core.h"
#include "vdec.h"
#include "venc.h"

static void venus_sys_error_handler(struct work_struct *work)
{
	struct venus_core *core =
			container_of(work, struct venus_core, work.work);
	int ret;

	hfi_core_deinit(core);

	mutex_lock(&core->lock);

	rproc_report_crash(core->rproc, RPROC_FATAL_ERROR);
	rproc_shutdown(core->rproc);

	ret = rproc_boot(core->rproc);
	if (ret)
		goto exit;

	enable_irq(core->irq);

	core->state = CORE_INIT;

exit:
	mutex_unlock(&core->lock);
}

static void venus_event_notify(struct venus_core *core, u32 event)
{
	struct venus_inst *inst;

	switch (event) {
	case EVT_SYS_WATCHDOG_TIMEOUT:
	case EVT_SYS_ERROR:
		break;
	default:
		return;
	}

	mutex_lock(&core->lock);
	core->sys_error = true;
	list_for_each_entry(inst, &core->instances, list)
		inst->ops->event_notify(inst, EVT_SESSION_ERROR, NULL);
	mutex_unlock(&core->lock);

	disable_irq_nosync(core->irq);

	/*
	 * Sleep for 5 sec to ensure venus has completed any pending cache
	 * operations. Without this sleep, we see device reset when firmware is
	 * unloaded after a system error.
	 */
	schedule_delayed_work(&core->work, msecs_to_jiffies(5000));
}

static const struct hfi_core_ops venus_core_ops = {
	.event_notify = venus_event_notify,
};

static int venus_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct venus_core *core = video_drvdata(file);
	struct venus_inst *inst;
	int ret;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	INIT_LIST_HEAD(&inst->registeredbufs);
	INIT_LIST_HEAD(&inst->internalbufs);
	INIT_LIST_HEAD(&inst->list);
	mutex_init(&inst->lock);

	inst->core = core;

	if (vdev == core->vdev_dec) {
		inst->session_type = VIDC_SESSION_TYPE_DEC;
		ret = vdec_open(inst);
		if (ret)
			goto err_free_inst;
		v4l2_fh_init(&inst->fh, core->vdev_dec);
	} else {
		inst->session_type = VIDC_SESSION_TYPE_ENC;
		ret = venc_open(inst);
		if (ret)
			goto err_free_inst;
		v4l2_fh_init(&inst->fh, core->vdev_enc);
	}

	inst->fh.ctrl_handler = &inst->ctrl_handler;
	v4l2_fh_add(&inst->fh);
	inst->fh.m2m_ctx = inst->m2m_ctx;
	file->private_data = &inst->fh;

	return 0;

err_free_inst:
	kfree(inst);
	return ret;
}

static int venus_close(struct file *file)
{
	struct venus_inst *inst = to_inst(file);

	if (inst->session_type == VIDC_SESSION_TYPE_DEC)
		vdec_close(inst);
	else
		venc_close(inst);

	mutex_destroy(&inst->lock);

	v4l2_fh_del(&inst->fh);
	v4l2_fh_exit(&inst->fh);

	kfree(inst);
	return 0;
}

static const struct v4l2_file_operations venus_fops = {
	.owner = THIS_MODULE,
	.open = venus_open,
	.release = venus_close,
	.unlocked_ioctl = video_ioctl2,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = v4l2_compat_ioctl32,
#endif
};

static int venus_clks_get(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	struct device *dev = core->dev;
	unsigned int i;

	for (i = 0; i < res->clks_num; i++) {
		core->clks[i] = devm_clk_get(dev, res->clks[i]);
		if (IS_ERR(core->clks[i]))
			return PTR_ERR(core->clks[i]);
	}

	return 0;
}

static int venus_clks_enable(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	unsigned int i;
	int ret;

	for (i = 0; i < res->clks_num; i++) {
		ret = clk_prepare_enable(core->clks[i]);
		if (ret)
			goto err;
	}

	return 0;
err:
	while (--i)
		clk_disable_unprepare(core->clks[i]);

	return ret;
}

static void venus_clks_disable(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	unsigned int i;

	for (i = 0; i < res->clks_num; i++)
		clk_disable_unprepare(core->clks[i]);
}

static int venus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct venus_core *core;
	struct device_node *rproc;
	struct resource *r;
	int ret;

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;
	platform_set_drvdata(pdev, core);

	rproc = of_parse_phandle(dev->of_node, "rproc", 0);
	if (IS_ERR(rproc))
		return PTR_ERR(rproc);

	core->rproc = rproc_get_by_phandle(rproc->phandle);
	if (IS_ERR(core->rproc))
		return PTR_ERR(core->rproc);
	else if (!core->rproc)
		return -EPROBE_DEFER;

	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "base");
	core->base = devm_ioremap_resource(dev, r);
	if (IS_ERR(core->base))
		return PTR_ERR(core->base);

	core->irq = platform_get_irq_byname(pdev, "venus");
	if (core->irq < 0)
		return core->irq;

	core->res = of_device_get_match_data(dev);
	if (!core->res)
		return -ENODEV;

	ret = venus_clks_get(core);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, core->res->dma_mask);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&core->instances);
	mutex_init(&core->lock);
	INIT_DELAYED_WORK(&core->work, venus_sys_error_handler);

	ret = devm_request_threaded_irq(dev, core->irq, hfi_isr, hfi_isr_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"venus", core);
	if (ret)
		return ret;

	core->core_ops = &venus_core_ops;

	ret = hfi_create(core);
	if (ret)
		return ret;

	ret = venus_clks_enable(core);
	if (ret)
		goto err_hfi_destroy;

	ret = rproc_boot(core->rproc);
	if (ret) {
		venus_clks_disable(core);
		goto err_hfi_destroy;
	}

	venus_clks_disable(core);

	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto err_runtime_disable;

	ret = hfi_core_init(core);
	if (ret)
		goto err_rproc_shutdown;

	ret = pm_runtime_put_sync(dev);
	if (ret)
		goto err_core_deinit;

	ret = v4l2_device_register(dev, &core->v4l2_dev);
	if (ret)
		goto err_core_deinit;

	core->vdev_dec = video_device_alloc();
	if (!core->vdev_dec) {
		ret = -ENOMEM;
		goto err_dev_unregister;
	}

	core->vdev_enc = video_device_alloc();
	if (!core->vdev_enc) {
		ret = -ENOMEM;
		goto err_dec_release;
	}

	ret = vdec_init(core, core->vdev_dec, &venus_fops);
	if (ret)
		goto err_enc_release;

	ret = venc_init(core, core->vdev_enc, &venus_fops);
	if (ret)
		goto err_vdec_deinit;

	return 0;

err_vdec_deinit:
	vdec_deinit(core, core->vdev_dec);
err_enc_release:
	video_device_release(core->vdev_enc);
err_dec_release:
	video_device_release(core->vdev_dec);
err_dev_unregister:
	v4l2_device_unregister(&core->v4l2_dev);
err_core_deinit:
	hfi_core_deinit(core);
err_rproc_shutdown:
	rproc_shutdown(core->rproc);
err_runtime_disable:
	pm_runtime_set_suspended(dev);
	pm_runtime_disable(dev);
err_hfi_destroy:
	hfi_destroy(core);
	return ret;
}

static int venus_remove(struct platform_device *pdev)
{
	struct venus_core *core = platform_get_drvdata(pdev);
	int ret;

	pm_runtime_get_sync(&pdev->dev);

	ret = hfi_core_deinit(core);

	rproc_shutdown(core->rproc);

	pm_runtime_put_sync(&pdev->dev);

	hfi_destroy(core);
	vdec_deinit(core, core->vdev_dec);
	venc_deinit(core, core->vdev_enc);
	v4l2_device_unregister(&core->v4l2_dev);

	pm_runtime_disable(core->dev);

	return ret;
}

#ifdef CONFIG_PM
static int venus_runtime_suspend(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret;

	ret = hfi_core_suspend(core);

	venus_clks_disable(core);

	return ret;
}

static int venus_runtime_resume(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret;

	ret = venus_clks_enable(core);
	if (ret)
		return ret;

	return hfi_core_resume(core);
}
#endif

static const struct dev_pm_ops venus_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(venus_runtime_suspend, venus_runtime_resume, NULL)
};

static const struct freq_tbl msm8916_freq_table[] = {
	{ 352800, 228570000 },	/* 1920x1088 @ 30 + 1280x720 @ 30 */
	{ 244800, 160000000 },	/* 1920x1088 @ 30 */
	{ 108000, 100000000 },	/* 1280x720 @ 30 */
};

static const struct reg_val msm8916_reg_preset[] = {
	{ 0xe0020, 0x05555556 },
	{ 0xe0024, 0x05555556 },
	{ 0x80124, 0x00000003 },
};

static const struct venus_resources msm8916_res = {
	.freq_tbl = msm8916_freq_table,
	.freq_tbl_size = ARRAY_SIZE(msm8916_freq_table),
	.reg_tbl = msm8916_reg_preset,
	.reg_tbl_size = ARRAY_SIZE(msm8916_reg_preset),
	.clks = { "core", "iface", "bus", },
	.clks_num = 3,
	.max_load = 352800, /* 720p@30 + 1080p@30 */
	.hfi_version = HFI_VERSION_LEGACY,
	.vmem_id = VIDC_RESOURCE_NONE,
	.vmem_size = 0,
	.vmem_addr = 0,
	.dma_mask = 0xddc00000 - 1,
};

static const struct freq_tbl msm8996_freq_table[] = {
	{ 1944000, 490000000 },	/* 4k UHD @ 60 */
	{  972000, 320000000 },	/* 4k UHD @ 30 */
	{  489600, 150000000 },	/* 1080p @ 60 */
	{  244800,  75000000 },	/* 1080p @ 30 */
};

static const struct reg_val msm8996_reg_preset[] = {
	{ 0x80010, 0xffffffff },
	{ 0x80018, 0x00001556 },
	{ 0x8001C, 0x00001556 },
};

static const struct venus_resources msm8996_res = {
	.freq_tbl = msm8996_freq_table,
	.freq_tbl_size = ARRAY_SIZE(msm8996_freq_table),
	.reg_tbl = msm8996_reg_preset,
	.reg_tbl_size = ARRAY_SIZE(msm8996_reg_preset),
	.clks = { "core", "core0", "core1", "iface", "bus", "rpm_mmaxi",
		  "mmagic_ahb", "mmagic_maxi", "mmagic_video_axi", "maxi" },
	.clks_num = 10,
	.max_load = 2563200,
	.hfi_version = HFI_VERSION_3XX,
	.vmem_id = VIDC_RESOURCE_NONE,
	.vmem_size = 0,
	.vmem_addr = 0,
	.dma_mask = 0xddc00000 - 1,
};

static const struct of_device_id venus_dt_match[] = {
	{ .compatible = "qcom,msm8916-venus", .data = &msm8916_res, },
	{ .compatible = "qcom,msm8996-venus", .data = &msm8996_res, },
	{ }
};
MODULE_DEVICE_TABLE(of, venus_dt_match);

static struct platform_driver qcom_venus_driver = {
	.probe = venus_probe,
	.remove = venus_remove,
	.driver = {
		.name = "qcom-venus",
		.of_match_table = venus_dt_match,
		.pm = &venus_pm_ops,
	},
};
module_platform_driver(qcom_venus_driver);

MODULE_ALIAS("platform:qcom-venus");
MODULE_DESCRIPTION("Qualcomm Venus video encoder and decoder driver");
MODULE_LICENSE("GPL v2");
