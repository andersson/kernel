// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2016, The Linux Foundation
 * Copyright (c) 2018, Linaro Limited
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include "q6afe.h"

struct q6afe_dai_data {
	struct q6afe_port *port[AFE_PORT_MAX];
	struct q6afe_port_config port_config[AFE_PORT_MAX];
	bool is_port_started[AFE_PORT_MAX];
};

static int q6hdmi_format_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct q6afe_dai_data *dai_data = kcontrol->private_data;
	int value = ucontrol->value.integer.value[0];

	dai_data->port_config[AFE_PORT_HDMI_RX].hdmi.datatype = value;

	return 0;
}

static int q6hdmi_format_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{

	struct q6afe_dai_data *dai_data = kcontrol->private_data;

	ucontrol->value.integer.value[0] =
		dai_data->port_config[AFE_PORT_HDMI_RX].hdmi.datatype;

	return 0;
}

static const char * const hdmi_format[] = {
	"LPCM",
	"Compr"
};

static const struct soc_enum hdmi_config_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, hdmi_format),
};

static const struct snd_kcontrol_new q6afe_config_controls[] = {
	SOC_ENUM_EXT("HDMI RX Format", hdmi_config_enum[0],
				 q6hdmi_format_get,
				 q6hdmi_format_put),
};

static int q6slim_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{

	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	struct q6afe_slim_cfg *slim = &dai_data->port_config[dai->id].slim;

	slim->num_channels = params_channels(params);
	slim->sample_rate = params_rate(params);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_SPECIAL:
		slim->bit_width = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		slim->bit_width = 24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		slim->bit_width = 32;
		break;
	default:
		pr_err("%s: format %d\n",
			__func__, params_format(params));
		return -EINVAL;
	}

	return 0;
}

static int q6hdmi_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	int channels = params_channels(params);
	struct q6afe_hdmi_cfg *hdmi = &dai_data->port_config[dai->id].hdmi;

	hdmi->sample_rate = params_rate(params);
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		hdmi->bit_width = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		hdmi->bit_width = 24;
		break;
	}

	/*refer to HDMI spec CEA-861-E: Table 28 Audio InfoFrame Data Byte 4*/
	switch (channels) {
	case 2:
		hdmi->channel_allocation = 0;
		break;
	case 3:
		hdmi->channel_allocation = 0x02;
		break;
	case 4:
		hdmi->channel_allocation = 0x06;
		break;
	case 5:
		hdmi->channel_allocation = 0x0A;
		break;
	case 6:
		hdmi->channel_allocation = 0x0B;
		break;
	case 7:
		hdmi->channel_allocation = 0x12;
		break;
	case 8:
		hdmi->channel_allocation = 0x13;
		break;
	default:
		dev_err(dai->dev, "invalid Channels = %u\n", channels);
		return -EINVAL;
	}

	return 0;
}

static int q6i2s_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params,
			   struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	struct q6afe_i2s_cfg *i2s = &dai_data->port_config[dai->id].i2s_cfg;


	i2s->sample_rate = params_rate(params);
	i2s->bit_width = params_width(params);
	i2s->num_channels = params_channels(params);

	return 0;
}

static int q6i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	struct q6afe_i2s_cfg *i2s = &dai_data->port_config[dai->id].i2s_cfg;

	i2s->fmt = fmt;

	return 0;
}

static int q6afe_dai_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);

	dai_data->is_port_started[dai->id] = false;

	return 0;
}

static void q6afe_dai_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	int rc;

	rc = q6afe_port_stop(dai_data->port[dai->id]);
	if (rc < 0)
		dev_err(dai->dev, "fail to close AFE port\n");

	dai_data->is_port_started[dai->id] = false;

}

static int q6afe_mi2s_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	int rc;

	if (dai_data->is_port_started[dai->id]) {
		/* stop the port and restart with new port config */
		rc = q6afe_port_stop(dai_data->port[dai->id]);
		if (rc < 0) {
			dev_err(dai->dev, "fail to close AFE port\n");
			return rc;
		}
	}

	q6afe_i2s_port_prepare(dai_data->port[dai->id],
			       &dai_data->port_config[dai->id].i2s_cfg);

	rc = q6afe_port_start(dai_data->port[dai->id]);
	if (rc < 0) {
		dev_err(dai->dev, "fail to start AFE port %x\n", dai->id);
		return rc;
	}
	dai_data->is_port_started[dai->id] = true;

	return 0;
}

static int q6afe_dai_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	int rc;

	if (dai_data->is_port_started[dai->id]) {
		/* stop the port and restart with new port config */
		rc = q6afe_port_stop(dai_data->port[dai->id]);
		if (rc < 0) {
			dev_err(dai->dev, "fail to close AFE port\n");
			return rc;
		}
	}

	if (dai->id == AFE_PORT_HDMI_RX)
		q6afe_hdmi_port_prepare(dai_data->port[dai->id],
					&dai_data->port_config[dai->id].hdmi);
	else if (dai->id >= SLIMBUS_0_RX && dai->id <= SLIMBUS_6_TX)
		q6afe_slim_port_prepare(dai_data->port[dai->id],
					&dai_data->port_config[dai->id].slim);

	rc = q6afe_port_start(dai_data->port[dai->id]);
	if (rc < 0) {
		dev_err(dai->dev, "fail to start AFE port %x\n", dai->id);
		return rc;
	}
	dai_data->is_port_started[dai->id] = true;

	return 0;
}

static int q6slim_set_channel_map(struct snd_soc_dai *dai,
				unsigned int tx_num, unsigned int *tx_slot,
				unsigned int rx_num, unsigned int *rx_slot)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	struct q6afe_port_config *pcfg = &dai_data->port_config[dai->id];
	int i;

	if (!rx_slot) {
		pr_err("%s: rx slot not found\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < rx_num; i++) {
		pcfg->slim.ch_mapping[i] =   rx_slot[i];
		pr_debug("%s: find number of channels[%d] ch[%d]\n",
		       __func__, i, rx_slot[i]);
	}

	pcfg->slim.num_channels = rx_num;

	pr_debug("%s: SLIMBUS_%d_RX cnt[%d] ch[%d %d]\n", __func__,
		(dai->id - SLIMBUS_0_RX) / 2, rx_num,
		pcfg->slim.ch_mapping[0],
		pcfg->slim.ch_mapping[1]);

	return 0;
}

static int q6afe_mi2s_set_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	struct q6afe_port *port = dai_data->port[dai->id];

	switch (clk_id) {
	case LPAIF_DIG_CLK:
		return q6afe_port_set_sysclk(port, clk_id, 0, 5, freq, dir);
	case LPAIF_BIT_CLK:
	case LPAIF_OSR_CLK:
		return q6afe_port_set_sysclk(port, clk_id,
					     Q6AFE_LPASS_CLK_SRC_INTERNAL,
					     Q6AFE_LPASS_CLK_ROOT_DEFAULT,
					     freq, dir);
	}

	return 0;
}

static const struct snd_soc_dapm_route q6afe_dapm_routes[] = {
	{"HDMI Playback", NULL, "HDMI_RX"},
	{"Slimbus1 Playback", NULL, "SLIMBUS_1_RX"},
	{"Slimbus2 Playback", NULL, "SLIMBUS_2_RX"},
	{"Slimbus3 Playback", NULL, "SLIMBUS_3_RX"},
	{"Slimbus4 Playback", NULL, "SLIMBUS_4_RX"},
	{"Slimbus5 Playback", NULL, "SLIMBUS_5_RX"},
	{"Slimbus6 Playback", NULL, "SLIMBUS_6_RX"},

	{"Primary MI2S Playback", NULL, "PRI_MI2S_RX"},
	{"Secondary MI2S Playback", NULL, "SEC_MI2S_RX"},
	{"Tertiary MI2S Playback", NULL, "TERT_MI2S_RX"},
	{"Quaternary MI2S Playback", NULL, "QUAT_MI2S_RX"},
};

static struct snd_soc_dai_ops q6hdmi_ops = {
	.prepare	= q6afe_dai_prepare,
	.hw_params	= q6hdmi_hw_params,
	.shutdown	= q6afe_dai_shutdown,
	.startup	= q6afe_dai_startup,
};

static struct snd_soc_dai_ops q6i2s_ops = {
	.prepare	= q6afe_mi2s_prepare,
	.hw_params	= q6i2s_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.shutdown	= q6afe_dai_shutdown,
	.startup	= q6afe_dai_startup,
	.set_sysclk	= q6afe_mi2s_set_sysclk,
};

static struct snd_soc_dai_ops q6slim_ops = {
	.prepare	= q6afe_dai_prepare,
	.hw_params	= q6slim_hw_params,
	.shutdown	= q6afe_dai_shutdown,
	.startup	= q6afe_dai_startup,
	.set_channel_map = q6slim_set_channel_map,
};

static int msm_dai_q6_dai_probe(struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);
	struct snd_soc_dapm_context *dapm;
	struct q6afe_port *port;

	dapm = snd_soc_component_get_dapm(dai->component);

	port = q6afe_port_get_from_id(dai->dev, dai->id);
	if (IS_ERR(port)) {
		dev_err(dai->dev, "Unable to get afe port\n");
		return -EINVAL;
	}
	dai_data->port[dai->id] = port;

	return 0;
}

static int msm_dai_q6_dai_remove(struct snd_soc_dai *dai)
{
	struct q6afe_dai_data *dai_data = q6afe_get_dai_data(dai->dev);

	q6afe_port_put(dai_data->port[dai->id]);

	return 0;
}

static struct snd_soc_dai_driver q6afe_dais[] = {
	{
		.playback = {
			.stream_name = "HDMI Playback",
			.rates = SNDRV_PCM_RATE_48000 |
				 SNDRV_PCM_RATE_96000 |
			 SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_max =     192000,
			.rate_min =	48000,
		},
		.ops = &q6hdmi_ops,
		.id = AFE_PORT_HDMI_RX,
		.name = "HDMI",
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.name = "SLIMBUS_0_RX",
		.ops = &q6slim_ops,
		.id = SLIMBUS_0_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
		.playback = {
			.stream_name = "Slimbus Playback",
			.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 192000,
		},
	}, {
		.playback = {
			.stream_name = "Slimbus1 Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.name = "SLIMBUS_1_RX",
		.ops = &q6slim_ops,
		.id = SLIMBUS_1_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Slimbus2 Playback",
			.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.name = "SLIMBUS_2_RX",
		.ops = &q6slim_ops,
		.id = SLIMBUS_2_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Slimbus3 Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.name = "SLIMBUS_3_RX",
		.ops = &q6slim_ops,
		.id = SLIMBUS_3_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Slimbus4 Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.name = "SLIMBUS_4_RX",
		.ops = &q6slim_ops,
		.id = SLIMBUS_4_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Slimbus5 Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
			 SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.name = "SLIMBUS_5_RX",
		.ops = &q6slim_ops,
		.id = SLIMBUS_5_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Slimbus6 Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_44100,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.ops = &q6slim_ops,
		.name = "SLIMBUS_6_RX",
		.id = SLIMBUS_6_RX,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Primary MI2S Playback",
			.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.id = PRIMARY_MI2S_RX,
		.name = "PRI_MI2S_RX",
		.ops = &q6i2s_ops,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Secondary MI2S Playback",
			.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.name = "SEC_MI2S_RX",
		.id = SECONDARY_MI2S_RX,
		.ops = &q6i2s_ops,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Tertiary MI2S Playback",
			.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.name = "TERT_MI2S_RX",
		.id = TERTIARY_MI2S_RX,
		.ops = &q6i2s_ops,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	}, {
		.playback = {
			.stream_name = "Quaternary MI2S Playback",
			.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.name = "QUAT_MI2S_RX",
		.id = QUATERNARY_MI2S_RX,
		.ops = &q6i2s_ops,
		.probe = msm_dai_q6_dai_probe,
		.remove = msm_dai_q6_dai_remove,
	},
};

static int q6afe_of_xlate_dai_name(struct snd_soc_component *component,
				   struct of_phandle_args *args,
				   const char **dai_name)
{
	int id = args->args[0];
	int i, ret = -EINVAL;

	for (i = 0; i  < ARRAY_SIZE(q6afe_dais); i++) {
		if (q6afe_dais[i].id == id) {
			*dai_name = q6afe_dais[i].name;
			ret = 0;
			break;
		}
	}

	return ret;
}

static const struct snd_soc_dapm_widget q6afe_dai_widgets[] = {
	SND_SOC_DAPM_AIF_OUT("HDMI_RX", "HDMI Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_0_RX", "Slimbus Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_1_RX", "Slimbus1 Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_2_RX", "Slimbus2 Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_3_RX", "Slimbus3 Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_4_RX", "Slimbus4 Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_5_RX", "Slimbus5 Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLIMBUS_6_RX", "Slimbus6 Playback", 0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("QUAT_MI2S_RX", "Quaternary MI2S Playback",
						0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TERT_MI2S_RX", "Tertiary MI2S Playback",
						0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SEC_MI2S_RX", "Secondary MI2S Playback",
			     0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SEC_MI2S_RX_SD1",
			"Secondary MI2S Playback SD1",
			0, 0, 0, 0),
	SND_SOC_DAPM_AIF_OUT("PRI_MI2S_RX", "Primary MI2S Playback",
			     0, 0, 0, 0),
};

static const struct snd_soc_component_driver q6afe_dai_component = {
	.name		= "q6afe-dai-component",
	.dapm_widgets = q6afe_dai_widgets,
	.num_dapm_widgets = ARRAY_SIZE(q6afe_dai_widgets),
	.controls = q6afe_config_controls,
	.num_controls = ARRAY_SIZE(q6afe_config_controls),
	.dapm_routes = q6afe_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(q6afe_dapm_routes),
	.of_xlate_dai_name = q6afe_of_xlate_dai_name,

};

int q6afe_dai_dev_probe(struct device *dev)
{
	int rc = 0;
	struct q6afe_dai_data *dai_data;

	dai_data = devm_kzalloc(dev, sizeof(*dai_data), GFP_KERNEL);
	if (!dai_data)
		rc = -ENOMEM;

	q6afe_set_dai_data(dev, dai_data);

	return  devm_snd_soc_register_component(dev, &q6afe_dai_component,
					  q6afe_dais, ARRAY_SIZE(q6afe_dais));
}
EXPORT_SYMBOL_GPL(q6afe_dai_dev_probe);

int q6afe_dai_dev_remove(struct device *dev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(q6afe_dai_dev_remove);
MODULE_DESCRIPTION("Q6 Audio Fronend dai driver");
MODULE_LICENSE("GPL v2");
