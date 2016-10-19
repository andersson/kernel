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
#ifndef __VENUS_VDEC_H__
#define __VENUS_VDEC_H__

struct venus_core;
struct video_device;
struct venus_inst;
struct v4l2_file_operations;

int vdec_init(struct venus_core *core, struct video_device *dec,
	      const struct v4l2_file_operations *fops);
void vdec_deinit(struct venus_core *core, struct video_device *dec);
int vdec_open(struct venus_inst *inst);
void vdec_close(struct venus_inst *inst);

int vdec_ctrl_init(struct venus_inst *inst);
void vdec_ctrl_deinit(struct venus_inst *inst);

#endif
