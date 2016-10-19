/*
 * Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
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
#ifndef __VENUS_HELPERS_H__
#define __VENUS_HELPERS_H__

#include <media/videobuf2-v4l2.h>

struct venus_inst;

struct vb2_v4l2_buffer *helper_find_buf(struct venus_inst *inst,
					unsigned int type, u32 idx);
void helper_buffers_done(struct venus_inst *inst, enum vb2_buffer_state state);
int helper_vb2_buf_init(struct vb2_buffer *vb);
int helper_vb2_buf_prepare(struct vb2_buffer *vb);
void helper_vb2_buf_queue(struct vb2_buffer *vb);
void helper_vb2_stop_streaming(struct vb2_queue *q);
int helper_vb2_start_streaming(struct venus_inst *inst);
void helper_m2m_device_run(void *priv);
void helper_m2m_job_abort(void *priv);
int helper_get_bufreq(struct venus_inst *inst, u32 type,
		      struct hfi_buffer_requirements *req);
int helper_set_input_resolution(struct venus_inst *inst, unsigned int width,
				unsigned int height);
int helper_set_output_resolution(struct venus_inst *inst, unsigned int width,
				 unsigned int height);
int helper_set_num_bufs(struct venus_inst *inst, unsigned int input_bufs,
			unsigned int output_bufs);
int helper_set_color_format(struct venus_inst *inst, u32 fmt);
#endif
