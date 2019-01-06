// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Ltd
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/soc/qcom/aoss-qmp.h>
#include <dt-bindings/power/qcom-aoss-qmp.h>

/* Requests are expected to be 96 bytes long */
#define AOSS_QMP_PD_MSG_LEN	96

struct qmp_pd {
	struct qmp *qmp;
	struct generic_pm_domain pd;
};

#define to_qmp_pd_resource(res) container_of(res, struct qmp_pd, pd)

struct qmp_pd_resource {
	const char *name;
	int (*on)(struct generic_pm_domain *domain);
	int (*off)(struct generic_pm_domain *domain);
};

static int qmp_pd_clock_toggle(struct qmp_pd *res, bool enable)
{
	char buf[AOSS_QMP_PD_MSG_LEN];

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "{class: clock, res: %s, val: %d}",
		 res->pd.name, enable);
	return qmp_send(res->qmp, buf, sizeof(buf));
}

static int qmp_pd_clock_on(struct generic_pm_domain *domain)
{
	return qmp_pd_clock_toggle(to_qmp_pd_resource(domain), true);
}

static int qmp_pd_clock_off(struct generic_pm_domain *domain)
{
	return qmp_pd_clock_toggle(to_qmp_pd_resource(domain), false);
}

static int qmp_pd_image_toggle(struct qmp_pd *res, bool enable)
{
	char buf[AOSS_QMP_PD_MSG_LEN];

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf),
		 "{class: image, res: load_state, name: %s, val: %s}",
		 res->pd.name, enable ? "on" : "off");
	return qmp_send(res->qmp, buf, sizeof(buf));
}

static int qmp_pd_image_on(struct generic_pm_domain *domain)
{
	return qmp_pd_image_toggle(to_qmp_pd_resource(domain), true);
}

static int qmp_pd_image_off(struct generic_pm_domain *domain)
{
	return qmp_pd_image_toggle(to_qmp_pd_resource(domain), false);
}

static const struct qmp_pd_resource sdm845_resources[] = {
	[AOSS_QMP_QDSS_CLK] = { "qdss", qmp_pd_clock_on, qmp_pd_clock_off },
	[AOSS_QMP_LS_CDSP] = { "cdsp", qmp_pd_image_on, qmp_pd_image_off },
	[AOSS_QMP_LS_LPASS] = { "adsp", qmp_pd_image_on, qmp_pd_image_off },
	[AOSS_QMP_LS_MODEM] = { "modem", qmp_pd_image_on, qmp_pd_image_off },
	[AOSS_QMP_LS_SLPI] = { "slpi", qmp_pd_image_on, qmp_pd_image_off },
	[AOSS_QMP_LS_SPSS] = { "spss", qmp_pd_image_on, qmp_pd_image_off },
	[AOSS_QMP_LS_VENUS] = { "venus", qmp_pd_image_on, qmp_pd_image_off },
};

static int qmp_pd_probe(struct platform_device *pdev)
{
	struct genpd_onecell_data *data;
	struct device *parent = pdev->dev.parent;
	struct qmp_pd *res;
	struct qmp *qmp;
	size_t num = ARRAY_SIZE(sdm845_resources);
	int ret;
	int i;

	qmp = dev_get_drvdata(pdev->dev.parent);
	if (!qmp)
		return -EINVAL;

	res = devm_kcalloc(&pdev->dev, num, sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->domains = devm_kcalloc(&pdev->dev, num, sizeof(*data->domains),
				     GFP_KERNEL);
	if (!data->domains)
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		res[i].qmp = qmp;
		res[i].pd.name = sdm845_resources[i].name;
		res[i].pd.power_on = sdm845_resources[i].on;
		res[i].pd.power_off = sdm845_resources[i].off;

		ret = pm_genpd_init(&res[i].pd, NULL, true);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed to init genpd\n");
			goto unroll_genpds;
		}

		data->domains[i] = &res[i].pd;
	}

	data->num_domains = i;

	platform_set_drvdata(pdev, data);

	return of_genpd_add_provider_onecell(parent->of_node, data);

unroll_genpds:
	for (i--; i >= 0; i--)
		pm_genpd_remove(data->domains[i]);

	return ret;
}

static int qmp_pd_remove(struct platform_device *pdev)
{
	struct device *parent = pdev->dev.parent;
	struct genpd_onecell_data *data = platform_get_drvdata(pdev);
	int i;

	of_genpd_del_provider(parent->of_node);

	for (i = 0; i < data->num_domains; i++)
		pm_genpd_remove(data->domains[i]);

	return 0;
}

static struct platform_driver qmp_pd_driver = {
	.driver = {
		.name		= "aoss_qmp_pd",
	},
	.probe = qmp_pd_probe,
	.remove = qmp_pd_remove,
};
module_platform_driver(qmp_pd_driver);

MODULE_ALIAS("platform:aoss_qmp_pd");
MODULE_DESCRIPTION("Qualcomm AOSS QMP load-state driver");
MODULE_LICENSE("GPL v2");
