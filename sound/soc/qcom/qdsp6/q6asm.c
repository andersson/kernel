// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2017, The Linux Foundation
 * Copyright (c) 2018, Linaro Limited
 */

#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/soc/qcom/apr.h>
#include <linux/device.h>
#include <linux/of.h>
#include <uapi/sound/asound.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "q6asm.h"
#include "q6dsp-errno.h"
#include "q6dsp-common.h"

#define ASM_STREAM_CMD_CLOSE			0x00010BCD
#define ASM_STREAM_CMD_FLUSH			0x00010BCE
#define ASM_SESSION_CMD_PAUSE			0x00010BD3
#define ASM_DATA_CMD_EOS			0x00010BDB
#define ASM_DEFAULT_POPP_TOPOLOGY		0x00010BE4
#define ASM_STREAM_CMD_FLUSH_READBUFS		0x00010C09
#define ASM_CMD_SHARED_MEM_MAP_REGIONS		0x00010D92
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00010D93
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS	0x00010D94
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2	0x00010D98
#define ASM_DATA_EVENT_WRITE_DONE_V2		0x00010D99
#define ASM_SESSION_CMD_RUN_V2			0x00010DAA
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2	0x00010DA5
#define ASM_DATA_CMD_WRITE_V2			0x00010DAB
#define ASM_SESSION_CMD_SUSPEND			0x00010DEC
#define ASM_STREAM_CMD_OPEN_WRITE_V3		0x00010DB3

#define ASM_LEGACY_STREAM_SESSION	0
#define ASM_END_POINT_DEVICE_MATRIX	0
#define ASM_DEFAULT_APP_TYPE		0
#define ASM_SYNC_IO_MODE		0x0001
#define ASM_ASYNC_IO_MODE		0x0002
#define ASM_TUN_READ_IO_MODE		0x0004	/* tunnel read write mode */
#define ASM_TUN_WRITE_IO_MODE		0x0008	/* tunnel read write mode */
#define ASM_SHIFT_GAPLESS_MODE_FLAG	31
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL	3

struct avs_cmd_shared_mem_map_regions {
	struct apr_hdr hdr;
	u16 mem_pool_id;
	u16 num_regions;
	u32 property_flag;
} __packed;

struct avs_shared_map_region_payload {
	u32 shm_addr_lsw;
	u32 shm_addr_msw;
	u32 mem_size_bytes;
} __packed;

struct avs_cmd_shared_mem_unmap_regions {
	struct apr_hdr hdr;
	u32 mem_map_handle;
} __packed;

struct asm_data_cmd_media_fmt_update_v2 {
	u32 fmt_blk_size;
} __packed;

struct asm_multi_channel_pcm_fmt_blk_v2 {
	struct apr_hdr hdr;
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u16 num_channels;
	u16 bits_per_sample;
	u32 sample_rate;
	u16 is_signed;
	u16 reserved;
	u8 channel_mapping[PCM_FORMAT_MAX_NUM_CHANNEL];
} __packed;

struct asm_data_cmd_write_v2 {
	struct apr_hdr hdr;
	u32 buf_addr_lsw;
	u32 buf_addr_msw;
	u32 mem_map_handle;
	u32 buf_size;
	u32 seq_id;
	u32 timestamp_lsw;
	u32 timestamp_msw;
	u32 flags;
} __packed;

struct asm_stream_cmd_open_write_v3 {
	struct apr_hdr hdr;
	uint32_t mode_flags;
	uint16_t sink_endpointype;
	uint16_t bits_per_sample;
	uint32_t postprocopo_id;
	uint32_t dec_fmt_id;
} __packed;

struct asm_session_cmd_run_v2 {
	struct apr_hdr hdr;
	u32 flags;
	u32 time_lsw;
	u32 time_msw;
} __packed;

struct audio_buffer {
	phys_addr_t phys;
	uint32_t used;
	uint32_t size;		/* size of buffer */
};

struct audio_port_data {
	struct audio_buffer *buf;
	uint32_t num_periods;
	uint32_t dsp_buf;
	uint32_t mem_map_handle;
};

struct audio_client {
	int session;
	q6asm_cb cb;
	int cmd_state;
	void *priv;
	uint32_t io_mode;
	struct apr_device *adev;
	struct mutex lock;
	/* idx:1 out port, 0: in port */
	struct audio_port_data port[2];
	wait_queue_head_t cmd_wait;
	int perf_mode;
	int stream_id;
	struct device *dev;
};

struct q6asm {
	struct apr_device *adev;
	int mem_state;
	struct device *dev;
	wait_queue_head_t mem_wait;
	struct mutex	session_lock;
	struct platform_device *pcmdev;
	struct audio_client *session[MAX_SESSIONS + 1];
	void *dai_data;
};

static bool q6asm_is_valid_audio_client(struct audio_client *ac)
{
	struct q6asm *a = dev_get_drvdata(ac->dev);
	int n;

	if (!ac)
		return false;

	for (n = 1; n <= MAX_SESSIONS; n++) {
		if (a->session[n] == ac)
			return true;
	}

	return false;
}

static inline void q6asm_add_hdr(struct audio_client *ac, struct apr_hdr *hdr,
				 uint32_t pkt_size, bool cmd_flg,
				 uint32_t stream_id)
{
	hdr->hdr_field = APR_SEQ_CMD_HDR_FIELD;
	hdr->src_svc = ac->adev->svc_id;
	hdr->src_domain = APR_DOMAIN_APPS;
	hdr->dest_svc = APR_SVC_ASM;
	hdr->dest_domain = APR_DOMAIN_ADSP;
	hdr->src_port = ((ac->session << 8) & 0xFF00) | (stream_id);
	hdr->dest_port = ((ac->session << 8) & 0xFF00) | (stream_id);
	hdr->pkt_size = pkt_size;
	if (cmd_flg)
		hdr->token = ac->session;
}

static int q6asm_apr_send_session_pkt(struct q6asm *a, struct audio_client *ac,
				      void *data)
{
	int rc;

	mutex_lock(&a->session_lock);
	a->mem_state = 1;
	rc = apr_send_pkt(a->adev, data);
	if (rc < 0)
		goto err;

	rc = wait_event_timeout(a->mem_wait, (a->mem_state <= 0), 5 * HZ);
	if (!rc) {
		dev_err(a->dev, "CMD timeout\n");
		rc = -ETIMEDOUT;
	} else if (a->mem_state < 0) {
		rc =  q6dsp_errno(a->mem_state);
	}

err:
	mutex_unlock(&a->session_lock);
	return rc;
}

static int __q6asm_memory_unmap(struct audio_client *ac,
				phys_addr_t buf_add, int dir)
{
	struct avs_cmd_shared_mem_unmap_regions mem_unmap;
	struct q6asm *a = dev_get_drvdata(ac->dev);
	int rc;

	if (ac->port[dir].mem_map_handle == 0) {
		dev_err(ac->dev, "invalid mem handle\n");
		return -EINVAL;
	}

	mem_unmap.hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	mem_unmap.hdr.src_port = 0;
	mem_unmap.hdr.dest_port = 0;
	mem_unmap.hdr.pkt_size = sizeof(mem_unmap);
	mem_unmap.hdr.token = ((ac->session << 8) | dir);

	mem_unmap.hdr.opcode = ASM_CMD_SHARED_MEM_UNMAP_REGIONS;
	mem_unmap.mem_map_handle = ac->port[dir].mem_map_handle;

	rc = q6asm_apr_send_session_pkt(a, ac, &mem_unmap);
	if (rc < 0)
		return rc;

	ac->port[dir].mem_map_handle = 0;

	return 0;
}

/**
 * q6asm_unmap_memory_regions() - unmap memory regions in the dsp.
 *
 * @dir: direction of audio stream
 * @ac: audio client instanace
 *
 * Return: Will be an negative value on failure or zero on success
 */
int q6asm_unmap_memory_regions(unsigned int dir, struct audio_client *ac)
{
	struct audio_port_data *port;
	int cnt = 0;
	int rc = 0;

	mutex_lock(&ac->lock);
	port = &ac->port[dir];
	if (!port->buf) {
		rc = -EINVAL;
		goto err;
	}
	cnt = port->num_periods - 1;
	if (cnt >= 0) {
		rc = __q6asm_memory_unmap(ac, port->buf[dir].phys, dir);
		if (rc < 0) {
			dev_err(ac->dev, "%s: Memory_unmap_regions failed %d\n",
				__func__, rc);
			goto err;
		}
	}

	port->num_periods = 0;
	kfree(port->buf);
	port->buf = NULL;

err:
	mutex_unlock(&ac->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(q6asm_unmap_memory_regions);

static int __q6asm_memory_map_regions(struct audio_client *ac, int dir,
				      size_t period_sz, unsigned int periods,
				      bool is_contiguous)
{
	struct avs_cmd_shared_mem_map_regions *cmd = NULL;
	struct avs_shared_map_region_payload *mregions = NULL;
	struct q6asm *a = dev_get_drvdata(ac->dev);
	struct audio_port_data *port = NULL;
	struct audio_buffer *ab = NULL;
	void *mmap_region_cmd = NULL;
	uint32_t num_regions, buf_sz;
	int rc, i, cmd_size;

	num_regions = is_contiguous ? 1 : periods;
	buf_sz = is_contiguous ? (period_sz * periods) : period_sz;
	buf_sz = PAGE_ALIGN(buf_sz);

	cmd_size = sizeof(*cmd) + (sizeof(*mregions) * num_regions);
	mmap_region_cmd = kzalloc(cmd_size, GFP_KERNEL);
	if (!mmap_region_cmd)
		return -ENOMEM;

	cmd = mmap_region_cmd;

	cmd->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	cmd->hdr.src_port = 0;
	cmd->hdr.dest_port = 0;
	cmd->hdr.pkt_size = cmd_size;
	cmd->hdr.token = ((ac->session << 8) | dir);


	cmd->hdr.opcode = ASM_CMD_SHARED_MEM_MAP_REGIONS;
	cmd->mem_pool_id = ADSP_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = num_regions;
	cmd->property_flag = 0x00;

	mregions = mmap_region_cmd +  sizeof(*cmd);

	port = &ac->port[dir];

	for (i = 0; i < num_regions; i++) {
		ab = &port->buf[i];
		mregions->shm_addr_lsw = lower_32_bits(ab->phys);
		mregions->shm_addr_msw = upper_32_bits(ab->phys);
		mregions->mem_size_bytes = buf_sz;
		++mregions;
	}

	rc = q6asm_apr_send_session_pkt(a, ac, mmap_region_cmd);

	kfree(mmap_region_cmd);

	return rc;
}

/**
 * q6asm_map_memory_regions() - map memory regions in the dsp.
 *
 * @dir: direction of audio stream
 * @ac: audio client instanace
 * @phys: physcial address that needs mapping.
 * @period_sz: audio period size
 * @periods: number of periods
 *
 * Return: Will be an negative value on failure or zero on success
 */
int q6asm_map_memory_regions(unsigned int dir, struct audio_client *ac,
			     phys_addr_t phys,
			     size_t period_sz, unsigned int periods)
{
	struct audio_buffer *buf;
	int cnt;
	int rc;

	mutex_lock(&ac->lock);

	if (ac->port[dir].buf) {
		dev_err(ac->dev, "Buffer already allocated\n");
		rc = 0;
		goto err;
	}


	buf = kzalloc(((sizeof(struct audio_buffer)) * periods), GFP_KERNEL);
	if (!buf) {
		rc = -ENOMEM;
		goto err;
	}


	ac->port[dir].buf = buf;

	buf[0].phys = phys;
	buf[0].used = !!dir;
	buf[0].size = period_sz;

	for (cnt = 1; cnt < periods; cnt++) {
		if (period_sz > 0) {
			buf[cnt].phys = buf[0].phys + (cnt * period_sz);
			buf[cnt].used = dir ^ 1;
			buf[cnt].size = period_sz;
		}
	}

	ac->port[dir].num_periods = periods;

	rc = __q6asm_memory_map_regions(ac, dir, period_sz, periods, 1);
	if (rc < 0) {
		dev_err(ac->dev, "Memory_map_regions failed\n");
		ac->port[dir].num_periods = 0;
		kfree(buf);
		ac->port[dir].buf = NULL;
		goto err;
	}

err:
	mutex_unlock(&ac->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(q6asm_map_memory_regions);

/**
 * q6asm_audio_client_free() - Freee allocated audio client
 *
 * @ac: audio client to free
 */
void q6asm_audio_client_free(struct audio_client *ac)
{
	struct q6asm *a = dev_get_drvdata(ac->dev);

	mutex_lock(&a->session_lock);
	a->session[ac->session] = NULL;
	mutex_unlock(&a->session_lock);
	kfree(ac);
}
EXPORT_SYMBOL_GPL(q6asm_audio_client_free);

static struct audio_client *q6asm_get_audio_client(struct q6asm *a,
						   int session_id)
{
	if ((session_id <= 0) || (session_id > MAX_SESSIONS)) {
		dev_err(a->dev, "invalid session: %d\n", session_id);
		return NULL;
	}

	if (!a->session[session_id]) {
		dev_err(a->dev, "session not active: %d\n", session_id);
		return NULL;
	}

	return a->session[session_id];
}

/**
 * q6asm_set_dai_data() - set dai private data
 *
 * @dev: Pointer to asm device.
 * @data: dai private data
 *
 */
void q6asm_set_dai_data(struct device *dev, void *data)
{
	struct q6asm *a = dev_get_drvdata(dev);

	a->dai_data = data;
}
EXPORT_SYMBOL_GPL(q6asm_set_dai_data);

/**
 * q6asm_get_dai_data() - get dai private data
 *
 * @dev: Pointer to asm device.
 *
 * Return: pointer to dai private data
 */
void *q6asm_get_dai_data(struct device *dev)
{
	struct q6asm *a = dev_get_drvdata(dev);

	return a->dai_data;
}
EXPORT_SYMBOL_GPL(q6asm_get_dai_data);

static int32_t q6asm_stream_callback(struct apr_device *adev,
				     struct apr_client_message *data,
				     int session_id)
{
	struct q6asm *q6asm = dev_get_drvdata(&adev->dev);
	struct aprv2_ibasic_rsp_result_t *result;
	struct audio_port_data *port;
	struct audio_client *ac;
	uint32_t token;
	uint32_t client_event = 0;

	ac = q6asm_get_audio_client(q6asm, session_id);
	if (!ac)/* Audio client might already be freed by now */
		return 0;

	if (!q6asm_is_valid_audio_client(ac))
		return -EINVAL;

	result = data->payload;

	switch (data->opcode) {
	case APR_BASIC_RSP_RESULT:
		token = data->token;
		switch (result->opcode) {
		case ASM_SESSION_CMD_PAUSE:
			client_event = ASM_CLIENT_EVENT_CMD_PAUSE_DONE;
			break;
		case ASM_SESSION_CMD_SUSPEND:
			client_event = ASM_CLIENT_EVENT_CMD_SUSPEND_DONE;
			break;
		case ASM_DATA_CMD_EOS:
			client_event = ASM_CLIENT_EVENT_CMD_EOS_DONE;
			break;
			break;
		case ASM_STREAM_CMD_FLUSH:
			client_event = ASM_CLIENT_EVENT_CMD_FLUSH_DONE;
			break;
		case ASM_SESSION_CMD_RUN_V2:
			client_event = ASM_CLIENT_EVENT_CMD_RUN_DONE;
			break;

		case ASM_STREAM_CMD_FLUSH_READBUFS:
			if (token != ac->session) {
				dev_err(ac->dev, "session invalid\n");
				return -EINVAL;
			}
		case ASM_STREAM_CMD_CLOSE:
			client_event = ASM_CLIENT_EVENT_CMD_CLOSE_DONE;
			break;
		case ASM_STREAM_CMD_OPEN_WRITE_V3:
		case ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2:
			if (result->status != 0) {
				dev_err(ac->dev,
					"cmd = 0x%x returned error = 0x%x\n",
					result->opcode, result->status);
				ac->cmd_state = -result->status;
				wake_up(&ac->cmd_wait);
				return 0;
			}
			break;
		default:
			dev_err(ac->dev, "command[0x%x] not expecting rsp\n",
				result->opcode);
			break;
		}

		if (ac->cmd_state) {
			ac->cmd_state = 0;
			wake_up(&ac->cmd_wait);
		}
		if (ac->cb)
			ac->cb(client_event, data->token,
			       data->payload, ac->priv);

		return 0;

	case ASM_DATA_EVENT_WRITE_DONE_V2:
		port =  &ac->port[SNDRV_PCM_STREAM_PLAYBACK];

		client_event = ASM_CLIENT_EVENT_DATA_WRITE_DONE;

		if (ac->io_mode & ASM_SYNC_IO_MODE) {
			phys_addr_t phys = port->buf[data->token].phys;

			if (lower_32_bits(phys) != result->opcode ||
			    upper_32_bits(phys) != result->status) {
				dev_err(ac->dev, "Expected addr %pa\n",
					&port->buf[data->token].phys);
				return -EINVAL;
			}
			token = data->token;
			port->buf[token].used = 1;
		}
		break;
	}

	if (ac->cb)
		ac->cb(client_event, data->token, data->payload, ac->priv);

	return 0;
}

static int q6asm_srvc_callback(struct apr_device *adev,
			       struct apr_client_message *data)
{
	struct aprv2_ibasic_rsp_result_t *result;
	struct q6asm *a, *q6asm = dev_get_drvdata(&adev->dev);
	struct audio_client *ac = NULL;
	struct audio_port_data *port;
	uint32_t dir = 0;
	uint32_t sid = 0;
	int session_id;

	session_id = (data->dest_port >> 8) & 0xFF;
	if (session_id)
		return q6asm_stream_callback(adev, data, session_id);

	result = data->payload;
	sid = (data->token >> 8) & 0x0F;
	ac = q6asm_get_audio_client(q6asm, sid);
	if (!ac) {
		dev_err(&adev->dev, "Audio Client not active\n");
		return 0;
	}

	a = dev_get_drvdata(ac->dev);
	dir = (data->token & 0x0F);
	port = &ac->port[dir];

	switch (data->opcode)
	case APR_BASIC_RSP_RESULT: {
		switch (result->opcode) {
		case ASM_CMD_SHARED_MEM_MAP_REGIONS:
		case ASM_CMD_SHARED_MEM_UNMAP_REGIONS:
			if (result->status != 0) {
				dev_err(ac->dev,
					"cmd = 0x%x retur err= 0x%x sid:%d\n",
					result->opcode, result->status, sid);
				a->mem_state = -result->status;
			} else {
				a->mem_state = 0;
			}

			wake_up(&a->mem_wait);
			break;
		default:
			dev_err(&adev->dev, "command[0x%x] not expecting rsp\n",
				 result->opcode);
			break;
		}
		return 0;
	case ASM_CMDRSP_SHARED_MEM_MAP_REGIONS:
		a->mem_state = 0;
		ac->port[dir].mem_map_handle = result->opcode;
		wake_up(&a->mem_wait);
		break;
	case ASM_CMD_SHARED_MEM_UNMAP_REGIONS:
		a->mem_state = 0;
		ac->port[dir].mem_map_handle = 0;
		wake_up(&a->mem_wait);
		break;
	default:
		dev_dbg(&adev->dev, "command[0x%x]success [0x%x]\n",
			result->opcode, result->status);
		break;
	}

	if (ac->cb)
		ac->cb(data->opcode, data->token, data->payload, ac->priv);

	return 0;
}

/**
 * q6asm_get_session_id() - get session id for audio client
 *
 * @ac: audio client pointer
 *
 * Return: Will be an session id of the audio client.
 */
int q6asm_get_session_id(struct audio_client *c)
{
	return c->session;
}
EXPORT_SYMBOL_GPL(q6asm_get_session_id);

/**
 * q6asm_audio_client_alloc() - Allocate a new audio client
 *
 * @dev: Pointer to asm child device.
 * @cb: event callback.
 * @priv: private data associated with this client.
 *
 * Return: Will be an error pointer on error or a valid audio client
 * on success.
 */
struct audio_client *q6asm_audio_client_alloc(struct device *dev, q6asm_cb cb,
					      void *priv, int stream_id)
{
	struct q6asm *a = dev_get_drvdata(dev);
	struct audio_client *ac;

	if (stream_id + 1 > MAX_SESSIONS)
		return ERR_PTR(-EINVAL);

	ac = kzalloc(sizeof(*ac), GFP_KERNEL);
	if (!ac)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&a->session_lock);
	a->session[stream_id + 1] = ac;
	mutex_unlock(&a->session_lock);

	ac->session = stream_id + 1;
	ac->cb = cb;
	ac->dev = dev;
	ac->priv = priv;
	ac->io_mode = ASM_SYNC_IO_MODE;
	ac->perf_mode = LEGACY_PCM_MODE;
	/* DSP expects stream id from 1 */
	ac->stream_id = 1;
	ac->adev = a->adev;

	init_waitqueue_head(&ac->cmd_wait);
	mutex_init(&ac->lock);
	ac->cmd_state = 0;

	return ac;
}
EXPORT_SYMBOL_GPL(q6asm_audio_client_alloc);

static int q6asm_ac_send_cmd_sync(struct audio_client *ac, void *cmd)
{
	int rc;

	mutex_lock(&ac->lock);
	ac->cmd_state = 1;

	rc = apr_send_pkt(ac->adev, cmd);
	if (rc < 0)
		goto err;

	rc = wait_event_timeout(ac->cmd_wait, (ac->cmd_state <= 0), 5 * HZ);
	if (!rc) {
		dev_err(ac->dev, "CMD timeout\n");
		rc =  -ETIMEDOUT;
		goto err;
	}

	if (ac->cmd_state > 0)
		rc = q6dsp_errno(ac->cmd_state);

err:
	mutex_unlock(&ac->lock);
	return rc;
}

/**
 * q6asm_open_write() - Open audio client for writing
 *
 * @ac: audio client pointer
 * @format: audio sample format
 * @bits_per_sample: bits per sample
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_open_write(struct audio_client *ac, uint32_t format,
		     uint16_t bits_per_sample)
{
	struct asm_stream_cmd_open_write_v3 open;
	int rc;

	q6asm_add_hdr(ac, &open.hdr, sizeof(open), true, ac->stream_id);

	open.hdr.opcode = ASM_STREAM_CMD_OPEN_WRITE_V3;
	open.mode_flags = 0x00;
	open.mode_flags |= ASM_LEGACY_STREAM_SESSION;

	/* source endpoint : matrix */
	open.sink_endpointype = ASM_END_POINT_DEVICE_MATRIX;
	open.bits_per_sample = bits_per_sample;
	open.postprocopo_id = ASM_DEFAULT_POPP_TOPOLOGY;

	switch (format) {
	case FORMAT_LINEAR_PCM:
		open.dec_fmt_id = ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2;
		break;
	default:
		dev_err(ac->dev, "Invalid format 0x%x\n", format);
		return -EINVAL;
	}

	rc = q6asm_ac_send_cmd_sync(ac, &open);
	if (rc < 0)
		return rc;

	ac->io_mode |= ASM_TUN_WRITE_IO_MODE;

	return 0;
}
EXPORT_SYMBOL_GPL(q6asm_open_write);

static int __q6asm_run(struct audio_client *ac, uint32_t flags,
	      uint32_t msw_ts, uint32_t lsw_ts, bool wait)
{
	struct asm_session_cmd_run_v2 run;

	q6asm_add_hdr(ac, &run.hdr, sizeof(run), true, ac->stream_id);

	run.hdr.opcode = ASM_SESSION_CMD_RUN_V2;
	run.flags = flags;
	run.time_lsw = lsw_ts;
	run.time_msw = msw_ts;
	if (wait)
		return q6asm_ac_send_cmd_sync(ac, &run);
	else
		return  apr_send_pkt(ac->adev, &run);

}

/**
 * q6asm_run() - start the audio client
 *
 * @ac: audio client pointer
 * @flags: flags associated with write
 * @msw_ts: timestamp msw
 * @lsw_ts: timestamp lsw
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_run(struct audio_client *ac, uint32_t flags,
	      uint32_t msw_ts, uint32_t lsw_ts)
{
	return __q6asm_run(ac, flags, msw_ts, lsw_ts, true);
}
EXPORT_SYMBOL_GPL(q6asm_run);

/**
 * q6asm_run_nowait() - start the audio client withou blocking
 *
 * @ac: audio client pointer
 * @flags: flags associated with write
 * @msw_ts: timestamp msw
 * @lsw_ts: timestamp lsw
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_run_nowait(struct audio_client *ac, uint32_t flags,
	      uint32_t msw_ts, uint32_t lsw_ts)
{
	return __q6asm_run(ac, flags, msw_ts, lsw_ts, false);
}
EXPORT_SYMBOL_GPL(q6asm_run_nowait);

/**
 * q6asm_media_format_block_multi_ch_pcm() - setup pcm configuration
 *
 * @ac: audio client pointer
 * @rate: audio sample rate
 * @channels: number of audio channels.
 * @use_default_chmap: flag to use default ch map.
 * @channel_map: channel map pointer
 * @bits_per_sample: bits per sample
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_media_format_block_multi_ch_pcm(struct audio_client *ac,
					  uint32_t rate, uint32_t channels,
					  u8 channel_map[PCM_FORMAT_MAX_NUM_CHANNEL],
					  uint16_t bits_per_sample)
{
	struct asm_multi_channel_pcm_fmt_blk_v2 fmt;
	u8 *channel_mapping;
	int rc;

	q6asm_add_hdr(ac, &fmt.hdr, sizeof(fmt), true, ac->stream_id);

	fmt.hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt.fmt_blk.fmt_blk_size = sizeof(fmt) - sizeof(fmt.hdr) -
	    sizeof(fmt.fmt_blk);
	fmt.num_channels = channels;
	fmt.bits_per_sample = bits_per_sample;
	fmt.sample_rate = rate;
	fmt.is_signed = 1;

	channel_mapping = fmt.channel_mapping;

	if (channel_map) {
		memcpy(channel_mapping, channel_map,
		       PCM_FORMAT_MAX_NUM_CHANNEL);
	} else {
		if (q6dsp_map_channels(channel_mapping, channels)) {
			dev_err(ac->dev, " map channels failed %d\n", channels);
			return -EINVAL;
		}
	}

	rc = q6asm_ac_send_cmd_sync(ac, &fmt);
	if (rc < 0)
		goto fail_cmd;

	return 0;
fail_cmd:
	return rc;
}
EXPORT_SYMBOL_GPL(q6asm_media_format_block_multi_ch_pcm);

/**
 * q6asm_write_async() - non blocking write
 *
 * @ac: audio client pointer
 * @len: lenght in bytes
 * @msw_ts: timestamp msw
 * @lsw_ts: timestamp lsw
 * @flags: flags associated with write
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_write_async(struct audio_client *ac, uint32_t len, uint32_t msw_ts,
		       uint32_t lsw_ts, uint32_t flags)
{
	struct asm_data_cmd_write_v2 write;
	struct audio_port_data *port;
	struct audio_buffer *ab;
	int rc = 0;

	if (!(ac->io_mode & ASM_SYNC_IO_MODE))
		return 0;

	port = &ac->port[SNDRV_PCM_STREAM_PLAYBACK];
	q6asm_add_hdr(ac, &write.hdr, sizeof(write), false,
		      ac->stream_id);

	ab = &port->buf[port->dsp_buf];

	write.hdr.token = port->dsp_buf;
	write.hdr.opcode = ASM_DATA_CMD_WRITE_V2;
	write.buf_addr_lsw = lower_32_bits(ab->phys);
	write.buf_addr_msw = upper_32_bits(ab->phys);
	write.buf_size = len;
	write.seq_id = port->dsp_buf;
	write.timestamp_lsw = lsw_ts;
	write.timestamp_msw = msw_ts;
	write.mem_map_handle =
	    ac->port[SNDRV_PCM_STREAM_PLAYBACK].mem_map_handle;

	if (flags == NO_TIMESTAMP)
		write.flags = (flags & 0x800000FF);
	else
		write.flags = (0x80000000 | flags);

	port->dsp_buf++;

	if (port->dsp_buf >= port->num_periods)
		port->dsp_buf = 0;

	rc = apr_send_pkt(ac->adev, &write);
	if (rc < 0)
		return rc;

	return 0;
}
EXPORT_SYMBOL_GPL(q6asm_write_async);

static void q6asm_reset_buf_state(struct audio_client *ac)
{
	int cnt = 0;
	int loopcnt = 0;
	int used;
	struct audio_port_data *port = NULL;

	if (!(ac->io_mode & ASM_SYNC_IO_MODE))
		return;

	used = (ac->io_mode & ASM_TUN_WRITE_IO_MODE ? 1 : 0);
	mutex_lock(&ac->lock);
	for (loopcnt = 0; loopcnt <= SNDRV_PCM_STREAM_CAPTURE;
	     loopcnt++) {
		port = &ac->port[loopcnt];
		cnt = port->num_periods - 1;
		port->dsp_buf = 0;
		while (cnt >= 0) {
			if (!port->buf)
				continue;
			port->buf[cnt].used = used;
			cnt--;
		}
	}
	mutex_unlock(&ac->lock);
}

static int __q6asm_cmd(struct audio_client *ac, int cmd, bool wait)
{
	int stream_id = ac->stream_id;
	struct apr_hdr hdr;
	int rc;

	q6asm_add_hdr(ac, &hdr, sizeof(hdr), true, stream_id);

	switch (cmd) {
	case CMD_PAUSE:
		hdr.opcode = ASM_SESSION_CMD_PAUSE;
		break;
	case CMD_SUSPEND:
		hdr.opcode = ASM_SESSION_CMD_SUSPEND;
		break;
	case CMD_FLUSH:
		hdr.opcode = ASM_STREAM_CMD_FLUSH;
		break;
	case CMD_OUT_FLUSH:
		hdr.opcode = ASM_STREAM_CMD_FLUSH_READBUFS;
		break;
	case CMD_EOS:
		hdr.opcode = ASM_DATA_CMD_EOS;
		break;
	case CMD_CLOSE:
		hdr.opcode = ASM_STREAM_CMD_CLOSE;
		break;
	default:
		return -EINVAL;
	}

	if (wait)
		rc = q6asm_ac_send_cmd_sync(ac, &hdr);
	else
		return apr_send_pkt(ac->adev, &hdr);

	if (rc < 0)
		return rc;

	if (cmd == CMD_FLUSH)
		q6asm_reset_buf_state(ac);

	return 0;
}

/**
 * q6asm_cmd() - run cmd on audio client
 *
 * @ac: audio client pointer
 * @cmd: command to run on audio client.
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_cmd(struct audio_client *ac, int cmd)
{
	return __q6asm_cmd(ac, cmd, true);
}
EXPORT_SYMBOL_GPL(q6asm_cmd);

/**
 * q6asm_cmd_nowait() - non blocking, run cmd on audio client
 *
 * @ac: audio client pointer
 * @cmd: command to run on audio client.
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_cmd_nowait(struct audio_client *ac, int cmd)
{
	return __q6asm_cmd(ac, cmd, false);
}
EXPORT_SYMBOL_GPL(q6asm_cmd_nowait);

static int q6asm_probe(struct apr_device *adev)
{
	struct q6asm *q6asm;

	q6asm = devm_kzalloc(&adev->dev, sizeof(*q6asm), GFP_KERNEL);
	if (!q6asm)
		return -ENOMEM;

	q6asm->dev = &adev->dev;
	q6asm->adev = adev;
	q6asm->mem_state = 0;
	init_waitqueue_head(&q6asm->mem_wait);
	mutex_init(&q6asm->session_lock);
	dev_set_drvdata(&adev->dev, q6asm);

	return q6asm_dai_probe(&adev->dev);
}

static int q6asm_remove(struct apr_device *adev)
{
	return q6asm_dai_remove(&adev->dev);
}

static const struct of_device_id q6asm_device_id[]  = {
	{ .compatible = "qcom,q6asm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6asm_device_id);

static struct apr_driver qcom_q6asm_driver = {
	.probe = q6asm_probe,
	.remove = q6asm_remove,
	.callback = q6asm_srvc_callback,
	.driver = {
		.name = "qcom-q6asm",
		.of_match_table = of_match_ptr(q6asm_device_id),
	},
};

module_apr_driver(qcom_q6asm_driver);
MODULE_DESCRIPTION("Q6 Audio Stream Manager driver");
MODULE_LICENSE("GPL v2");
