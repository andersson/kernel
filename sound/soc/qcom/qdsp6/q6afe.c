// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2017, The Linux Foundation
 * Copyright (c) 2018, Linaro Limited
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/soc/qcom/apr.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include "q6dsp-errno.h"
#include "q6afe.h"

/* AFE CMDs */
#define AFE_PORT_CMD_DEVICE_START	0x000100E5
#define AFE_PORT_CMD_DEVICE_STOP	0x000100E6
#define AFE_PORT_CMD_SET_PARAM_V2	0x000100EF
#define AFE_PORT_CMDRSP_GET_PARAM_V2	0x00010106
#define AFE_PARAM_ID_HDMI_CONFIG	0x00010210
#define AFE_MODULE_AUDIO_DEV_INTERFACE	0x0001020C

#define AFE_PARAM_ID_CDC_SLIMBUS_SLAVE_CFG 0x00010235

#define AFE_PARAM_ID_SLIMBUS_CONFIG    0x00010212
#define AFE_PARAM_ID_I2S_CONFIG	0x0001020D

/* I2S config specific */
#define AFE_API_VERSION_I2S_CONFIG	0x1
#define AFE_PORT_I2S_SD0		0x1
#define AFE_PORT_I2S_SD1		0x2
#define AFE_PORT_I2S_SD2		0x3
#define AFE_PORT_I2S_SD3		0x4
#define AFE_PORT_I2S_QUAD01		0x5
#define AFE_PORT_I2S_QUAD23		0x6
#define AFE_PORT_I2S_6CHS		0x7
#define AFE_PORT_I2S_8CHS		0x8
#define AFE_PORT_I2S_MONO		0x0
#define AFE_PORT_I2S_STEREO		0x1
#define AFE_PORT_CONFIG_I2S_WS_SRC_EXTERNAL	0x0
#define AFE_PORT_CONFIG_I2S_WS_SRC_INTERNAL	0x1
#define AFE_LINEAR_PCM_DATA				0x0


/* Port IDs */
#define AFE_API_VERSION_HDMI_CONFIG	0x1
#define AFE_PORT_ID_MULTICHAN_HDMI_RX	0x100E

#define AFE_API_VERSION_SLIMBUS_CONFIG 0x1

/* SLIMbus Rx port on channel 0. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_0_RX      0x4000
/* SLIMbus Tx port on channel 0. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_0_TX      0x4001
/* SLIMbus Rx port on channel 1. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_1_RX      0x4002
/* SLIMbus Tx port on channel 1. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_1_TX      0x4003
/* SLIMbus Rx port on channel 2. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_2_RX      0x4004
/* SLIMbus Tx port on channel 2. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_2_TX      0x4005
/* SLIMbus Rx port on channel 3. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_3_RX      0x4006
/* SLIMbus Tx port on channel 3. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_3_TX      0x4007
/* SLIMbus Rx port on channel 4. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_4_RX      0x4008
/* SLIMbus Tx port on channel 4. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_4_TX      0x4009
/* SLIMbus Rx port on channel 5. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_5_RX      0x400a
/* SLIMbus Tx port on channel 5. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_5_TX      0x400b
/* SLIMbus Rx port on channel 6. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_6_RX      0x400c
/* SLIMbus Tx port on channel 6. */
#define AFE_PORT_ID_SLIMBUS_MULTI_CHAN_6_TX      0x400d
#define AFE_PORT_ID_PRIMARY_MI2S_RX         0x1000
#define AFE_PORT_ID_PRIMARY_MI2S_TX         0x1001
#define AFE_PORT_ID_SECONDARY_MI2S_RX       0x1002
#define AFE_PORT_ID_SECONDARY_MI2S_TX       0x1003
#define AFE_PORT_ID_TERTIARY_MI2S_RX        0x1004
#define AFE_PORT_ID_TERTIARY_MI2S_TX        0x1005
#define AFE_PORT_ID_QUATERNARY_MI2S_RX      0x1006
#define AFE_PORT_ID_QUATERNARY_MI2S_TX      0x1007

#define TIMEOUT_MS 1000
#define AFE_CMD_RESP_AVAIL	0
#define AFE_CMD_RESP_NONE	1

struct q6afe {
	struct apr_device *apr;
	struct device *dev;
	int state;
	int status;

	struct mutex lock;
	struct list_head port_list;
	spinlock_t port_list_lock;
	struct list_head node;
	void *dai_data;
};

struct afe_port_cmd_device_start {
	struct apr_hdr hdr;
	u16 port_id;
	u16 reserved;
} __packed;

struct afe_port_cmd_device_stop {
	struct apr_hdr hdr;
	u16 port_id;
	u16 reserved;
/* Reserved for 32-bit alignment. This field must be set to 0.*/
} __packed;

struct afe_port_param_data_v2 {
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
} __packed;

struct afe_port_cmd_set_param_v2 {
	u16 port_id;
	u16 payload_size;
	u32 payload_address_lsw;
	u32 payload_address_msw;
	u32 mem_map_handle;
} __packed;

struct afe_param_id_hdmi_multi_chan_audio_cfg {
	u32 hdmi_cfg_minor_version;
	u16 datatype;
	u16 channel_allocation;
	u32 sample_rate;
	u16 bit_width;
	u16 reserved;
} __packed;

struct afe_param_id_slimbus_cfg {
	u32                  sb_cfg_minor_version;
/* Minor version used for tracking the version of the SLIMBUS
 * configuration interface.
 * Supported values: #AFE_API_VERSION_SLIMBUS_CONFIG
 */

	u16                  slimbus_dev_id;
/* SLIMbus hardware device ID, which is required to handle
 * multiple SLIMbus hardware blocks.
 * Supported values: - #AFE_SLIMBUS_DEVICE_1 - #AFE_SLIMBUS_DEVICE_2
 */
	u16                  bit_width;
/* Bit width of the sample.
 * Supported values: 16, 24
 */
	u16                  data_format;
/* Data format supported by the SLIMbus hardware. The default is
 * 0 (#AFE_SB_DATA_FORMAT_NOT_INDICATED), which indicates the
 * hardware does not perform any format conversions before the data
 * transfer.
 */
	u16                  num_channels;
/* Number of channels.
 * Supported values: 1 to #AFE_PORT_MAX_AUDIO_CHAN_CNT
 */
	u8  shared_ch_mapping[AFE_PORT_MAX_AUDIO_CHAN_CNT];
/* Mapping of shared channel IDs (128 to 255) to which the
 * master port is to be connected.
 * Shared_channel_mapping[i] represents the shared channel assigned
 * for audio channel i in multichannel audio data.
 */
	u32              sample_rate;
/* Sampling rate of the port.
 * Supported values:
 * - #AFE_PORT_SAMPLE_RATE_8K
 * - #AFE_PORT_SAMPLE_RATE_16K
 * - #AFE_PORT_SAMPLE_RATE_48K
 * - #AFE_PORT_SAMPLE_RATE_96K
 * - #AFE_PORT_SAMPLE_RATE_192K
 */
} __packed;

struct afe_param_id_i2s_cfg {
	u32	i2s_cfg_minor_version;
	u16	bit_width;
	u16	channel_mode;
	u16	mono_stereo;
	u16	ws_src;
	u32	sample_rate;
	u16	data_format;
	u16	reserved;
} __packed;

union afe_port_config {
	struct afe_param_id_hdmi_multi_chan_audio_cfg hdmi_multi_ch;
	struct afe_param_id_slimbus_cfg           slim_cfg;
	struct afe_param_id_i2s_cfg	i2s_cfg;
} __packed;

struct q6afe_port {
	wait_queue_head_t wait;
	union afe_port_config port_cfg;
	int token;
	int id;
	int cfg_type;
	struct q6afe *afe;
	struct list_head	node;
};

struct afe_audioif_config_command {
	struct apr_hdr hdr;
	struct afe_port_cmd_set_param_v2 param;
	struct afe_port_param_data_v2 pdata;
	union afe_port_config port;
} __packed;

struct afe_port_map {
	int port_id;
	int token;
	int is_rx;
	int is_dig_pcm;
};

/* Port map of index vs real hw port ids */
static struct afe_port_map port_maps[AFE_PORT_MAX] = {
	[AFE_PORT_HDMI_RX] = { AFE_PORT_ID_MULTICHAN_HDMI_RX,
				AFE_PORT_HDMI_RX, 1, 1},
	[SLIMBUS_0_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_0_RX,
				SLIMBUS_0_RX, 1, 1},
	[SLIMBUS_1_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_1_RX,
				SLIMBUS_1_RX, 1, 1},
	[SLIMBUS_2_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_2_RX,
				SLIMBUS_2_RX, 1, 1},
	[SLIMBUS_3_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_3_RX,
				SLIMBUS_3_RX, 1, 1},
	[SLIMBUS_4_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_4_RX,
				SLIMBUS_4_RX, 1, 1},
	[SLIMBUS_5_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_5_RX,
				SLIMBUS_5_RX, 1, 1},
	[QUATERNARY_MI2S_RX] = { AFE_PORT_ID_QUATERNARY_MI2S_RX,
				QUATERNARY_MI2S_RX, 1, 1},
	[SECONDARY_MI2S_RX] = { AFE_PORT_ID_SECONDARY_MI2S_RX,
				SECONDARY_MI2S_RX, 1, 1},
	[TERTIARY_MI2S_RX] = { AFE_PORT_ID_TERTIARY_MI2S_RX,
				TERTIARY_MI2S_RX, 1, 1},
	[PRIMARY_MI2S_RX] = { AFE_PORT_ID_PRIMARY_MI2S_RX,
				PRIMARY_MI2S_RX, 1, 1},
	[SLIMBUS_6_RX] = { AFE_PORT_ID_SLIMBUS_MULTI_CHAN_6_RX,
				SLIMBUS_6_RX, 1, 1},
};

static struct q6afe_port *afe_find_port(struct q6afe *afe, int token)
{
	struct q6afe_port *p = NULL;

	spin_lock(&afe->port_list_lock);
	list_for_each_entry(p, &afe->port_list, node)
		if (p->token == token)
			break;

	spin_unlock(&afe->port_list_lock);
	return p;
}

static int afe_callback(struct apr_device *adev,
			struct apr_client_message *data)
{
	struct q6afe *afe = dev_get_drvdata(&adev->dev);
	struct aprv2_ibasic_rsp_result_t *res;
	struct q6afe_port *port;

	if (!data->payload_size)
		return 0;

	res = data->payload;
	if (data->opcode == APR_BASIC_RSP_RESULT) {
		if (res->status) {
			afe->status = res->status;
			dev_err(afe->dev, "cmd = 0x%x returned error = 0x%x\n",
				res->opcode, res->status);
		}

		switch (res->opcode) {
		case AFE_PORT_CMD_SET_PARAM_V2:
		case AFE_PORT_CMD_DEVICE_STOP:
		case AFE_PORT_CMD_DEVICE_START:
			afe->state = AFE_CMD_RESP_AVAIL;
			port = afe_find_port(afe, data->token);
			if (port)
				wake_up(&port->wait);

			break;
		default:
			dev_err(afe->dev, "Unknown cmd 0x%x\n",	res->opcode);
			break;
		}
	}

	return 0;
}
/**
 * q6afe_get_port_id() - Get port id from a given port index
 *
 * @index: port index
 *
 * Return: Will be an negative on error or valid port_id on success
 */
int q6afe_get_port_id(int index)
{
	if (index < 0 || index > AFE_PORT_MAX)
		return -EINVAL;

	return port_maps[index].port_id;
}
EXPORT_SYMBOL_GPL(q6afe_get_port_id);

static int afe_apr_send_pkt(struct q6afe *afe, void *data,
			    wait_queue_head_t *wait)
{
	int ret;

	mutex_lock(&afe->lock);
	afe->status = 0;
	afe->state = AFE_CMD_RESP_NONE;

	ret = apr_send_pkt(afe->apr, data);
	if (ret < 0) {
		dev_err(afe->dev, "packet not transmitted\n");
		ret = -EINVAL;
		goto err;
	}

	ret = wait_event_timeout(*wait, (afe->state == AFE_CMD_RESP_AVAIL),
				 msecs_to_jiffies(TIMEOUT_MS));
	if (!ret) {
		ret = -ETIMEDOUT;
	} else if (afe->status > 0) {
		dev_err(afe->dev, "DSP returned error[%s]\n",
		       q6dsp_strerror(afe->status));
		ret = q6dsp_errno(afe->status);
	} else {
		ret = 0;
	}

err:
	mutex_unlock(&afe->lock);

	return ret;
}

static int afe_send_cmd_port_start(struct q6afe_port *port)
{
	u16 port_id = port->id;
	struct afe_port_cmd_device_start start;
	struct q6afe *afe = port->afe;
	int ret;

	start.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					    APR_HDR_LEN(APR_HDR_SIZE),
					    APR_PKT_VER);
	start.hdr.pkt_size = sizeof(start);
	start.hdr.src_port = 0;
	start.hdr.dest_port = 0;
	start.hdr.token = port->token;
	start.hdr.opcode = AFE_PORT_CMD_DEVICE_START;
	start.port_id = port_id;

	ret = afe_apr_send_pkt(afe, &start, &port->wait);
	if (ret)
		dev_err(afe->dev, "AFE enable for port 0x%x failed %d\n",
		       port_id, ret);

	return ret;
}

static int q6afe_port_set_param_v2(struct q6afe_port *port, void *data,
				   int param_id, int psize)
{
	struct apr_hdr *hdr;
	struct afe_port_cmd_set_param_v2 *param;
	struct afe_port_param_data_v2 *pdata;
	struct q6afe *afe = port->afe;
	u16 port_id = port->id;
	int ret;

	hdr = data;
	param = data + sizeof(*hdr);
	pdata = data + sizeof(*hdr) + sizeof(*param);

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					      APR_HDR_LEN(APR_HDR_SIZE),
					      APR_PKT_VER);
	hdr->pkt_size = sizeof(*hdr) + sizeof(*param) +
			sizeof(*pdata) + psize;
	hdr->src_port = 0;
	hdr->dest_port = 0;
	hdr->token = port->token;
	hdr->opcode = AFE_PORT_CMD_SET_PARAM_V2;
	param->port_id = port_id;
	param->payload_size = sizeof(*pdata) + psize;
	param->payload_address_lsw = 0x00;
	param->payload_address_msw = 0x00;
	param->mem_map_handle = 0x00;
	pdata->module_id = AFE_MODULE_AUDIO_DEV_INTERFACE;
	pdata->param_id = param_id;
	pdata->param_size = psize;

	ret = afe_apr_send_pkt(afe, data, &port->wait);
	if (ret)
		dev_err(afe->dev, "AFE enable for port 0x%x failed %d\n",
		       port_id, ret);


	return ret;
}

static int afe_port_start(struct q6afe_port *port,
			  union afe_port_config *afe_config)
{
	struct afe_audioif_config_command config = {0,};
	struct q6afe *afe = port->afe;
	int port_id = port->id;
	int ret, param_id = port->cfg_type;

	config.port = *afe_config;

	ret  = q6afe_port_set_param_v2(port, &config, param_id,
				       sizeof(*afe_config));
	if (ret) {
		dev_err(afe->dev, "AFE enable for port 0x%x failed %d\n",
			port_id, ret);
		return ret;
	}
	return afe_send_cmd_port_start(port);
}

/**
 * q6afe_port_stop() - Stop a afe port
 *
 * @port: Instance of port to stop
 *
 * Return: Will be an negative on packet size on success.
 */
int q6afe_port_stop(struct q6afe_port *port)
{
	int port_id = port->id;
	struct afe_port_cmd_device_stop stop;
	struct q6afe *afe = port->afe;
	int ret = 0;
	int index = 0;

	port_id = port->id;
	index = port->token;
	if (index < 0 || index > AFE_PORT_MAX) {
		dev_err(afe->dev, "AFE port index[%d] invalid!\n", index);
		return -EINVAL;
	}

	stop.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	stop.hdr.pkt_size = sizeof(stop);
	stop.hdr.src_port = 0;
	stop.hdr.dest_port = 0;
	stop.hdr.token = index;
	stop.hdr.opcode = AFE_PORT_CMD_DEVICE_STOP;
	stop.port_id = port_id;
	stop.reserved = 0;

	ret = afe_apr_send_pkt(afe, &stop, &port->wait);
	if (ret)
		dev_err(afe->dev, "AFE close failed %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(q6afe_port_stop);

/**
 * q6afe_set_dai_data() - set dai private data
 *
 * @dev: Pointer to afe device.
 * @data: dai private data
 *
 */
void q6afe_set_dai_data(struct device *dev, void *data)
{
	struct q6afe *afe = dev_get_drvdata(dev);

	afe->dai_data = data;
}
EXPORT_SYMBOL_GPL(q6afe_set_dai_data);

/**
 * q6afe_get_dai_data() - get dai private data
 *
 * @dev: Pointer to afe device.
 *
 * Return: pointer to dai private data
 */
void *q6afe_get_dai_data(struct device *dev)
{
	struct q6afe *afe = dev_get_drvdata(dev);

	return afe->dai_data;
}
EXPORT_SYMBOL_GPL(q6afe_get_dai_data);

/**
 * q6afe_slim_port_prepare() - Prepare slim afe port.
 *
 * @port: Instance of afe port
 * @cfg: SLIM configuration for the afe port
 *
 */
void q6afe_slim_port_prepare(struct q6afe_port *port,
			     struct q6afe_slim_cfg *cfg)
{
	union afe_port_config *pcfg = &port->port_cfg;

	pcfg->slim_cfg.sb_cfg_minor_version = AFE_API_VERSION_SLIMBUS_CONFIG;
	pcfg->slim_cfg.sample_rate = cfg->sample_rate;
	pcfg->slim_cfg.bit_width = cfg->bit_width;
	pcfg->slim_cfg.num_channels = cfg->num_channels;
	pcfg->slim_cfg.data_format = cfg->data_format;
	pcfg->slim_cfg.shared_ch_mapping[0] = cfg->ch_mapping[0];
	pcfg->slim_cfg.shared_ch_mapping[1] = cfg->ch_mapping[1];
	pcfg->slim_cfg.shared_ch_mapping[2] = cfg->ch_mapping[2];
	pcfg->slim_cfg.shared_ch_mapping[3] = cfg->ch_mapping[3];

}
EXPORT_SYMBOL_GPL(q6afe_slim_port_prepare);

/**
 * q6afe_hdmi_port_prepare() - Prepare hdmi afe port.
 *
 * @port: Instance of afe port
 * @cfg: HDMI configuration for the afe port
 *
 */
void q6afe_hdmi_port_prepare(struct q6afe_port *port,
			     struct q6afe_hdmi_cfg *cfg)
{
	union afe_port_config *pcfg = &port->port_cfg;

	pcfg->hdmi_multi_ch.hdmi_cfg_minor_version =
					AFE_API_VERSION_HDMI_CONFIG;
	pcfg->hdmi_multi_ch.datatype = cfg->datatype;
	pcfg->hdmi_multi_ch.channel_allocation = cfg->channel_allocation;
	pcfg->hdmi_multi_ch.sample_rate = cfg->sample_rate;
	pcfg->hdmi_multi_ch.bit_width = cfg->bit_width;
}
EXPORT_SYMBOL_GPL(q6afe_hdmi_port_prepare);

/**
 * q6afe_i2s_port_prepare() - Prepare i2s afe port.
 *
 * @port: Instance of afe port
 * @cfg: I2S configuration for the afe port
 *
 */
void q6afe_i2s_port_prepare(struct q6afe_port *port, struct q6afe_i2s_cfg *cfg)
{
	union afe_port_config *pcfg = &port->port_cfg;

	pcfg->i2s_cfg.i2s_cfg_minor_version = AFE_API_VERSION_I2S_CONFIG;
	pcfg->i2s_cfg.sample_rate = cfg->sample_rate;
	pcfg->i2s_cfg.bit_width = cfg->bit_width;
	pcfg->i2s_cfg.data_format = AFE_LINEAR_PCM_DATA;

	switch (cfg->fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		pcfg->i2s_cfg.ws_src = AFE_PORT_CONFIG_I2S_WS_SRC_INTERNAL;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		/* CPU is slave */
		pcfg->i2s_cfg.ws_src = AFE_PORT_CONFIG_I2S_WS_SRC_EXTERNAL;
		break;
	default:
		break;
	}

	switch (cfg->num_channels) {
	case 1:
			pcfg->i2s_cfg.mono_stereo = AFE_PORT_I2S_MONO;
			pcfg->i2s_cfg.channel_mode = AFE_PORT_I2S_SD0;
		break;
	case 2:
			pcfg->i2s_cfg.channel_mode = AFE_PORT_I2S_SD0;
			pcfg->i2s_cfg.mono_stereo = AFE_PORT_I2S_STEREO;
		break;
	case 3:
	case 4:
			pcfg->i2s_cfg.channel_mode = AFE_PORT_I2S_QUAD01;
		break;
	case 5:
	case 6:
			pcfg->i2s_cfg.channel_mode = AFE_PORT_I2S_6CHS;
			break;
	case 7:
	case 8:
			pcfg->i2s_cfg.channel_mode = AFE_PORT_I2S_8CHS;
			break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(q6afe_i2s_port_prepare);

/**
 * q6afe_port_start() - Start a afe port
 *
 * @port: Instance of port to start
 *
 * Return: Will be an negative on packet size on success.
 */
int q6afe_port_start(struct q6afe_port *port)
{
	return afe_port_start(port, &port->port_cfg);
}
EXPORT_SYMBOL_GPL(q6afe_port_start);

/**
 * q6afe_port_get_from_id() - Get port instance from a port id
 *
 * @dev: Pointer to afe child device.
 * @id: port id
 *
 * Return: Will be an error pointer on error or a valid afe port
 * on success.
 */
struct q6afe_port *q6afe_port_get_from_id(struct device *dev, int id)
{
	int port_id;
	struct q6afe *afe = dev_get_drvdata(dev);
	struct q6afe_port *port;
	int cfg_type;

	if (id < 0 || id > AFE_PORT_MAX) {
		dev_err(dev, "AFE port token[%d] invalid!\n", id);
		return ERR_PTR(-EINVAL);
	}

	port_id = port_maps[id].port_id;

	switch (port_id) {
	case AFE_PORT_ID_MULTICHAN_HDMI_RX:
		cfg_type = AFE_PARAM_ID_HDMI_CONFIG;
		break;
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_0_RX:
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_1_RX:
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_2_RX:
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_3_RX:
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_4_RX:
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_5_RX:
	case AFE_PORT_ID_SLIMBUS_MULTI_CHAN_6_RX:
		cfg_type = AFE_PARAM_ID_SLIMBUS_CONFIG;
		break;

	case AFE_PORT_ID_PRIMARY_MI2S_RX:
	case AFE_PORT_ID_SECONDARY_MI2S_RX:
	case AFE_PORT_ID_TERTIARY_MI2S_RX:
	case AFE_PORT_ID_QUATERNARY_MI2S_RX:
		cfg_type = AFE_PARAM_ID_I2S_CONFIG;
		break;
	default:
		dev_err(dev, "Invalid port id 0x%x\n", port_id);
		return ERR_PTR(-EINVAL);
	}

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	init_waitqueue_head(&port->wait);

	port->token = id;
	port->id = port_id;
	port->afe = afe;
	port->cfg_type = cfg_type;

	spin_lock(&afe->port_list_lock);
	list_add_tail(&port->node, &afe->port_list);
	spin_unlock(&afe->port_list_lock);

	return port;

}
EXPORT_SYMBOL_GPL(q6afe_port_get_from_id);

/**
 * q6afe_port_put() - Release port reference
 *
 * @port: Instance of port to put
 */
void q6afe_port_put(struct q6afe_port *port)
{
	struct q6afe *afe = port->afe;

	spin_lock(&afe->port_list_lock);
	list_del(&port->node);
	spin_unlock(&afe->port_list_lock);
}
EXPORT_SYMBOL_GPL(q6afe_port_put);

static int q6afev2_probe(struct apr_device *adev)
{
	struct q6afe *afe;
	struct device *dev = &adev->dev;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	afe->apr = adev;
	mutex_init(&afe->lock);
	afe->dev = dev;
	INIT_LIST_HEAD(&afe->port_list);
	spin_lock_init(&afe->port_list_lock);

	dev_set_drvdata(dev, afe);

	return q6afe_dai_dev_probe(dev);
}

static int q6afev2_remove(struct apr_device *adev)
{
	return q6afe_dai_dev_remove(&adev->dev);
}

static const struct of_device_id q6afe_device_id[]  = {
	{ .compatible = "qcom,q6afe" },
	{},
};
MODULE_DEVICE_TABLE(of, q6afe_device_id);

static struct apr_driver qcom_q6afe_driver = {
	.probe = q6afev2_probe,
	.remove = q6afev2_remove,
	.callback = afe_callback,
	.driver = {
		.name = "qcom-q6afe",
		.of_match_table = of_match_ptr(q6afe_device_id),

	},
};

module_apr_driver(qcom_q6afe_driver);
MODULE_DESCRIPTION("Q6 Audio Front End");
MODULE_LICENSE("GPL v2");
