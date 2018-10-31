/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */
#ifndef _MSM_RMNET_H_
#define _MSM_RMNET_H_

/* IOCTL commands
 * Values chosen to not conflict with other drivers in the ecosystem
 */

#define RMNET_IOCTL_SET_LLP_IP	     0x000089f2 /* Set RAWIP protocol	  */
#define RMNET_IOCTL_OPEN	     0x000089f8 /* Open transport port	  */
#define RMNET_IOCTL_EXTENDED	     0x000089fd /* Extended IOCTLs	  */

/* Bitmap macros for RmNET driver operation mode. */
#define RMNET_MODE_QOS	    0x04

/* RmNet Data extended IOCTLs */
#define RMNET_IOCTL_GET_SUPPORTED_FEATURES     0x0000	/* Get features	   */
#define RMNET_IOCTL_GET_EPID		       0x0003	/* Get endpoint ID */
#define RMNET_IOCTL_SET_EGRESS_DATA_FORMAT     0x0006	/* Set EDF	   */
#define RMNET_IOCTL_SET_INGRESS_DATA_FORMAT    0x0007	/* Set IDF	   */
#define RMNET_IOCTL_GET_EP_PAIR		       0x0010	/* Endpoint pair   */

/* Return values for the RMNET_IOCTL_GET_SUPPORTED_FEATURES IOCTL */
#define RMNET_IOCTL_FEAT_NOTIFY_MUX_CHANNEL		BIT(0)
#define RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT		BIT(1)
#define RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT	BIT(2)

/* Input values for the RMNET_IOCTL_SET_EGRESS_DATA_FORMAT IOCTL  */
#define RMNET_IOCTL_EGRESS_FORMAT_AGGREGATION		BIT(2)
#define RMNET_IOCTL_EGRESS_FORMAT_CHECKSUM		BIT(4)

/* Input values for the RMNET_IOCTL_SET_INGRESS_DATA_FORMAT IOCTL */
#define RMNET_IOCTL_INGRESS_FORMAT_CHECKSUM		BIT(4)
#define RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA		BIT(5)

/* Return types for RMNET_IOCTL_EXTENDED requests */
struct rmnet_ioctl_extended_s {
	u32	extended_ioctl;
	union {
		u32	data; /* Generic data field for most extended IOCTLs */

		/* Return values for RMNET_IOCTL_GET_EP_PAIR */
		struct {
			u32	consumer_pipe_num;
			u32	producer_pipe_num;
		} ipa_endpoint_pair;

		struct {
			u32	__data; /* Placeholder for legacy data*/
			u32	agg_size;
			u32	agg_count;
		} ingress_format;
	} u;
};

#endif /* _MSM_RMNET_H_ */
