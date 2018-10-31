/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */
#ifndef _IPA_NETDEV_H_
#define _IPA_NETDEV_H_

struct ipa;
struct ipa_endpoint;
struct net_device;
struct sk_buff;

int ipa_netdev_setup(struct ipa *ipa);
void ipa_netdev_teardown(struct ipa *ipa);

void ipa_netdev_skb_rx(struct net_device *netdev, struct sk_buff *skb);

void ipa_netdev_suspend(struct net_device *netdev);
void ipa_netdev_resume(struct net_device *netdev);

#endif /* _IPA_NETDEV_H_ */
