// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2016, The Linux Foundation
 * Copyright (c) 2017, Linaro Limited
 */

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <sound/pcm_params.h>
#include "q6asm.h"
#include "q6routing.h"
#include "q6dsp-errno.h"

#define PLAYBACK_MIN_NUM_PERIODS    2
#define PLAYBACK_MAX_NUM_PERIODS   8
#define PLAYBACK_MAX_PERIOD_SIZE    65536
#define PLAYBACK_MIN_PERIOD_SIZE    128

enum stream_state {
	Q6ASM_STREAM_IDLE = 0,
	Q6ASM_STREAM_STOPPED,
	Q6ASM_STREAM_RUNNING,
};

struct q6asm_dai_rtd {
	struct snd_pcm_substream *substream;
	phys_addr_t phys;
	unsigned int pcm_size;
	unsigned int pcm_count;
	unsigned int pcm_irq_pos;       /* IRQ position */
	unsigned int periods;
	uint16_t bits_per_sample;
	uint16_t source; /* Encoding source bit mask */
	struct audio_client *audio_client;
	uint16_t session_id;
	enum stream_state state;
};

struct q6asm_dai_data {
	long long int sid;
};

static struct snd_pcm_hardware q6asm_dai_hardware_playback = {
	.info =                 (SNDRV_PCM_INFO_MMAP |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE),
	.rates =                SNDRV_PCM_RATE_8000_192000,
	.rate_min =             8000,
	.rate_max =             192000,
	.channels_min =         1,
	.channels_max =         8,
	.buffer_bytes_max =     (PLAYBACK_MAX_NUM_PERIODS *
				PLAYBACK_MAX_PERIOD_SIZE),
	.period_bytes_min =	PLAYBACK_MIN_PERIOD_SIZE,
	.period_bytes_max =     PLAYBACK_MAX_PERIOD_SIZE,
	.periods_min =          PLAYBACK_MIN_NUM_PERIODS,
	.periods_max =          PLAYBACK_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
	88200, 96000, 176400, 192000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

static void event_handler(uint32_t opcode, uint32_t token,
			  uint32_t *payload, void *priv)
{
	struct q6asm_dai_rtd *prtd = priv;
	struct snd_pcm_substream *substream = prtd->substream;

	switch (opcode) {
	case ASM_CLIENT_EVENT_CMD_RUN_DONE:
		q6asm_write_async(prtd->audio_client,
				   prtd->pcm_count, 0, 0, NO_TIMESTAMP);
		break;
	case ASM_CLIENT_EVENT_CMD_EOS_DONE:
		prtd->state = Q6ASM_STREAM_STOPPED;
		break;
	case ASM_CLIENT_EVENT_DATA_WRITE_DONE: {
		prtd->pcm_irq_pos += prtd->pcm_count;
		snd_pcm_period_elapsed(substream);
		if (prtd->state == Q6ASM_STREAM_RUNNING)
			q6asm_write_async(prtd->audio_client,
					   prtd->pcm_count, 0, 0, NO_TIMESTAMP);

		break;
		}
	default:
		break;
	}
}

static int q6asm_dai_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = substream->private_data;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	struct q6asm_dai_data *pdata;
	int ret;

	pdata = q6asm_get_dai_data(soc_prtd->platform->dev);
	if (!pdata)
		return -EINVAL;

	if (!prtd || !prtd->audio_client) {
		pr_err("%s: private data null or audio client freed\n",
			__func__);
		return -EINVAL;
	}

	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	prtd->pcm_irq_pos = 0;
	/* rate and channels are sent to audio driver */
	if (prtd->state) {
		/* clear the previous setup if any  */
		q6asm_cmd(prtd->audio_client, CMD_CLOSE);
		q6asm_unmap_memory_regions(substream->stream,
					   prtd->audio_client);
		q6routing_stream_close(soc_prtd->dai_link->id,
					 SNDRV_PCM_STREAM_PLAYBACK);
	}

	ret = q6asm_map_memory_regions(substream->stream, prtd->audio_client,
				       prtd->phys,
				       (prtd->pcm_size / prtd->periods),
				       prtd->periods);

	if (ret < 0) {
		pr_err("Audio Start: Buffer Allocation failed rc = %d\n",
							ret);
		return -ENOMEM;
	}

	ret = q6asm_open_write(prtd->audio_client, FORMAT_LINEAR_PCM,
			       prtd->bits_per_sample);
	if (ret < 0) {
		pr_err("%s: q6asm_open_write failed\n", __func__);
		q6asm_audio_client_free(prtd->audio_client);
		prtd->audio_client = NULL;
		return -ENOMEM;
	}

	prtd->session_id = q6asm_get_session_id(prtd->audio_client);
	ret = q6routing_stream_open(soc_prtd->dai_link->id, LEGACY_PCM_MODE,
				      prtd->session_id, substream->stream);
	if (ret) {
		pr_err("%s: stream reg failed ret:%d\n", __func__, ret);
		return ret;
	}

	ret = q6asm_media_format_block_multi_ch_pcm(
			prtd->audio_client, runtime->rate,
			runtime->channels, NULL,
			prtd->bits_per_sample);
	if (ret < 0)
		pr_info("%s: CMD Format block failed\n", __func__);

	prtd->state = Q6ASM_STREAM_RUNNING;

	return 0;
}

static int q6asm_dai_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		ret = q6asm_run_nowait(prtd->audio_client, 0, 0, 0);
		break;
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = q6asm_run_nowait(prtd->audio_client, 0, 0, 0);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		prtd->state = Q6ASM_STREAM_STOPPED;
		ret = q6asm_cmd_nowait(prtd->audio_client, CMD_EOS);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		ret = q6asm_cmd_nowait(prtd->audio_client, CMD_PAUSE);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int q6asm_dai_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = soc_prtd->cpu_dai;

	struct q6asm_dai_rtd *prtd;
	struct q6asm_dai_data *pdata;
	struct device *dev = soc_prtd->platform->dev;
	int ret = 0;
	int stream_id;

	stream_id = cpu_dai->driver->id;

	pdata = q6asm_get_dai_data(dev);
	if (!pdata) {
		pr_err("Platform data not found ..\n");
		return -EINVAL;
	}

	prtd = kzalloc(sizeof(struct q6asm_dai_rtd), GFP_KERNEL);
	if (prtd == NULL)
		return -ENOMEM;

	prtd->substream = substream;
	prtd->audio_client = q6asm_audio_client_alloc(dev,
				(q6asm_cb)event_handler, prtd, stream_id);
	if (!prtd->audio_client) {
		pr_info("%s: Could not allocate memory\n", __func__);
		kfree(prtd);
		return -ENOMEM;
	}

//	prtd->audio_client->dev = dev;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		runtime->hw = q6asm_dai_hardware_playback;

	ret = snd_pcm_hw_constraint_list(runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&constraints_sample_rates);
	if (ret < 0)
		pr_info("snd_pcm_hw_constraint_list failed\n");
	/* Ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		pr_info("snd_pcm_hw_constraint_integer failed\n");

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_pcm_hw_constraint_minmax(runtime,
			SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
			PLAYBACK_MIN_NUM_PERIODS * PLAYBACK_MIN_PERIOD_SIZE,
			PLAYBACK_MAX_NUM_PERIODS * PLAYBACK_MAX_PERIOD_SIZE);
		if (ret < 0) {
			pr_err("constraint for buffer bytes min max ret = %d\n",
									ret);
		}
	}

	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 32);
	if (ret < 0) {
		pr_err("constraint for period bytes step ret = %d\n",
								ret);
	}
	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 32);
	if (ret < 0) {
		pr_err("constraint for buffer bytes step ret = %d\n",
								ret);
	}

	runtime->private_data = prtd;

	snd_soc_set_runtime_hwparams(substream, &q6asm_dai_hardware_playback);

	runtime->dma_bytes = q6asm_dai_hardware_playback.buffer_bytes_max;


	if (pdata->sid < 0)
		prtd->phys = substream->dma_buffer.addr;
	else
		prtd->phys = substream->dma_buffer.addr | (pdata->sid << 32);

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	return 0;
}

static int q6asm_dai_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = substream->private_data;
	struct q6asm_dai_rtd *prtd = runtime->private_data;

	if (prtd->audio_client) {
		q6asm_cmd(prtd->audio_client, CMD_CLOSE);
		q6asm_unmap_memory_regions(substream->stream,
					   prtd->audio_client);
		q6asm_audio_client_free(prtd->audio_client);
	}
	q6routing_stream_close(soc_prtd->dai_link->id,
						SNDRV_PCM_STREAM_PLAYBACK);
	kfree(prtd);
	return 0;
}

static snd_pcm_uframes_t q6asm_dai_pointer(struct snd_pcm_substream *substream)
{

	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;

	if (prtd->pcm_irq_pos >= prtd->pcm_size)
		prtd->pcm_irq_pos = 0;

	return bytes_to_frames(runtime, (prtd->pcm_irq_pos));
}

static int q6asm_dai_mmap(struct snd_pcm_substream *substream,
				struct vm_area_struct *vma)
{

	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = substream->private_data;
	struct device *dev = soc_prtd->platform->dev->parent;

	return dma_mmap_coherent(dev, vma,
			runtime->dma_area, runtime->dma_addr,
			runtime->dma_bytes);
}

static int q6asm_dai_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;

	prtd->pcm_size = params_buffer_bytes(params);
	prtd->periods = params_periods(params);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		prtd->bits_per_sample = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		prtd->bits_per_sample = 24;
		break;
	}

	return 0;
}

static struct snd_pcm_ops q6asm_dai_ops = {
	.open           = q6asm_dai_open,
	.hw_params	= q6asm_dai_hw_params,
	.close          = q6asm_dai_close,
	.ioctl          = snd_pcm_lib_ioctl,
	.prepare        = q6asm_dai_prepare,
	.trigger        = q6asm_dai_trigger,
	.pointer        = q6asm_dai_pointer,
	.mmap		= q6asm_dai_mmap,
};

static int q6asm_dai_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm_substream *substream;
	struct of_phandle_args args;
	struct device_node *node;
	struct q6asm_dai_data *pdata;
	struct snd_pcm *pcm = rtd->pcm;
	struct device *dev;
	int size, ret;

	dev = rtd->platform->dev->parent;
	node = dev->of_node;
	pdata = q6asm_get_dai_data(rtd->platform->dev);

	ret = of_parse_phandle_with_fixed_args(node, "iommus", 1, 0, &args);
	if (ret < 0)
		pdata->sid = -1;
	else
		pdata->sid = args.args[0];



	substream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	size = q6asm_dai_hardware_playback.buffer_bytes_max;
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, dev, size,
				  &substream->dma_buffer);
	if (ret)
		dev_err(dev, "Cannot allocate buffer(s)\n");

	return ret;
}

static void q6asm_dai_pcm_free(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	int i;

	for (i = 0; i < ARRAY_SIZE(pcm->streams); i++) {
		substream = pcm->streams[i].substream;
		if (substream) {
			snd_dma_free_pages(&substream->dma_buffer);
			substream->dma_buffer.area = NULL;
			substream->dma_buffer.addr = 0;
		}
	}
}

static struct snd_soc_platform_driver q6asm_soc_platform = {
	.ops		= &q6asm_dai_ops,
	.pcm_new	= q6asm_dai_pcm_new,
	.pcm_free	= q6asm_dai_pcm_free,

};

static const struct snd_soc_dapm_route afe_pcm_routes[] = {
	{"MM_DL1",  NULL, "MultiMedia1 Playback" },
	{"MM_DL2",  NULL, "MultiMedia2 Playback" },
	{"MM_DL3",  NULL, "MultiMedia3 Playback" },
	{"MM_DL4",  NULL, "MultiMedia4 Playback" },
	{"MM_DL5",  NULL, "MultiMedia5 Playback" },
	{"MM_DL6",  NULL, "MultiMedia6 Playback" },
	{"MM_DL7",  NULL, "MultiMedia7 Playback" },

};

static int fe_dai_probe(struct snd_soc_dai *dai)
{
	struct snd_soc_dapm_context *dapm;

	dapm = snd_soc_component_get_dapm(dai->component);
	snd_soc_dapm_add_routes(dapm, afe_pcm_routes,
				ARRAY_SIZE(afe_pcm_routes));

	return 0;
}

static const struct snd_soc_component_driver q6asm_fe_dai_component = {
	.name		= "q6asm-fe-dai",
};

static struct snd_soc_dai_driver q6asm_fe_dais[] = {
	{
		.playback = {
			.stream_name = "MultiMedia1 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia1",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA1,
	},
	{
		.playback = {
			.stream_name = "MultiMedia2 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia2",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.playback = {
			.stream_name = "MultiMedia3 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia3",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA3,
	},
	{
		.playback = {
			.stream_name = "MultiMedia4 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia4",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA4,
	},
	{
		.playback = {
			.stream_name = "MultiMedia5 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia5",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA5,
	},
	{
		.playback = {
			.stream_name = "MultiMedia6 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia6",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA6,
	},
	{
		.playback = {
			.stream_name = "MultiMedia7 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia7",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA7,
	},
	{
		.playback = {
			.stream_name = "MultiMedia8 Playback",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =	192000,
		},
		.name = "MultiMedia8",
		.probe = fe_dai_probe,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA8,
	},
};

int q6asm_dai_probe(struct device *dev)
{
	struct q6asm_dai_data *pdata;
	int rc;

	pdata = devm_kzalloc(dev, sizeof(struct q6asm_dai_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;


	q6asm_set_dai_data(dev, pdata);

	rc = devm_snd_soc_register_platform(dev,  &q6asm_soc_platform);
	if (rc) {
		dev_err(dev, "err_dai_platform\n");
		return rc;
	}

	return devm_snd_soc_register_component(dev, &q6asm_fe_dai_component,
					q6asm_fe_dais,
					ARRAY_SIZE(q6asm_fe_dais));
}
EXPORT_SYMBOL_GPL(q6asm_dai_probe);

int q6asm_dai_remove(struct device *dev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(q6asm_dai_remove);

MODULE_DESCRIPTION("Q6ASM dai driver");
MODULE_LICENSE("GPL v2");
