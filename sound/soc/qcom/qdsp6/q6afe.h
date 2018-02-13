// SPDX-License-Identifier: GPL-2.0

#ifndef __Q6AFE_H__
#define __Q6AFE_H__

#include <dt-bindings/sound/qcom,q6afe.h>

#define AFE_PORT_MAX		48

#define MSM_AFE_PORT_TYPE_RX 0
#define MSM_AFE_PORT_TYPE_TX 1
#define AFE_MAX_PORTS AFE_PORT_MAX

#define AFE_MAX_CHAN_COUNT	8
#define AFE_PORT_MAX_AUDIO_CHAN_CNT	0x8

#define Q6AFE_LPASS_CLK_SRC_INTERNAL 1
#define Q6AFE_LPASS_CLK_ROOT_DEFAULT 0

#define LPAIF_DIG_CLK	1
#define LPAIF_BIT_CLK	2
#define LPAIF_OSR_CLK	3

struct q6afe_hdmi_cfg {
	u16                  datatype;
	u16                  channel_allocation;
	u32                  sample_rate;
	u16                  bit_width;
};

struct q6afe_slim_cfg {
	u32	sample_rate;
	u16	bit_width;
	u16	data_format;
	u16	num_channels;
	u8	ch_mapping[AFE_MAX_CHAN_COUNT];
};

struct q6afe_i2s_cfg {
	u32	sample_rate;
	u16	bit_width;
	u16	data_format;
	u16	num_channels;
	int fmt;
};

struct q6afe_port_config {
	struct q6afe_hdmi_cfg hdmi;
	struct q6afe_slim_cfg slim;
	struct q6afe_i2s_cfg i2s_cfg;
};

struct q6afe_port;
void q6afe_set_dai_data(struct device *dev, void *data);
void *q6afe_get_dai_data(struct device *dev);

int q6afe_dai_dev_probe(struct device *dev);
int q6afe_dai_dev_remove(struct device *dev);

struct q6afe_port *q6afe_port_get_from_id(struct device *dev, int id);
int q6afe_port_start(struct q6afe_port *port);
int q6afe_port_stop(struct q6afe_port *port);
void q6afe_port_put(struct q6afe_port *port);
int q6afe_get_port_id(int index);
void q6afe_hdmi_port_prepare(struct q6afe_port *port,
			    struct q6afe_hdmi_cfg *cfg);
void q6afe_slim_port_prepare(struct q6afe_port *port,
			  struct q6afe_slim_cfg *cfg);
void q6afe_i2s_port_prepare(struct q6afe_port *port, struct q6afe_i2s_cfg *cfg);

int q6afe_port_set_sysclk(struct q6afe_port *port, int clk_id,
			  int clk_src, int clk_root,
			  unsigned int freq, int dir);
#endif /* __Q6AFE_H__ */
