// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */

/* Modem Transport Network Driver. */

#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/if_rmnet.h>

#include "ipa.h"
#include "ipa_data.h"
#include "ipa_endpoint.h"
#include "ipa_mem.h"
#include "ipa_netdev.h"
#include "ipa_qmi.h"
#include "msm_rmnet.h"

#define IPA_NETDEV_NAME		"rmnet_ipa%d"

#define TAILROOM		0	/* for padding by mux layer */

#define IPA_NETDEV_TIMEOUT	10	/* seconds */

/** struct ipa_priv - IPA network device private data */
struct ipa_priv {
	struct ipa *ipa;
};

/** ipa_netdev_open() - Opens the modem network interface */
static int ipa_netdev_open(struct net_device *netdev)
{
	struct ipa_priv *priv = netdev_priv(netdev);
	struct ipa *ipa = priv->ipa;
	int ret;

	ret = ipa_endpoint_enable_one(ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]);
	if (ret)
		return ret;
	ret = ipa_endpoint_enable_one(ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]);
	if (ret)
		goto err_disable_tx;

	netif_start_queue(netdev);

	return 0;

err_disable_tx:
	ipa_endpoint_disable_one(ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]);

	return ret;
}

/** ipa_netdev_stop() - Stops the modem network interface. */
static int ipa_netdev_stop(struct net_device *netdev)
{
	struct ipa_priv *priv = netdev_priv(netdev);
	struct ipa *ipa = priv->ipa;

	netif_stop_queue(netdev);

	ipa_endpoint_disable_one(ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]);
	ipa_endpoint_disable_one(ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]);

	return 0;
}

/** ipa_netdev_xmit() - Transmits an skb.
 * @skb: skb to be transmitted
 * @dev: network device
 *
 * Return codes:
 * NETDEV_TX_OK: Success
 * NETDEV_TX_BUSY: Error while transmitting the skb. Try again later
 */
static int ipa_netdev_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct net_device_stats *stats = &netdev->stats;
	struct ipa_priv *priv = netdev_priv(netdev);
	struct ipa_endpoint *endpoint;
	struct ipa *ipa = priv->ipa;
	u32 skb_len = skb->len;

	if (!skb_len)
		goto err_drop;

	endpoint = ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX];
	if (endpoint->data->config.qmap && skb->protocol != htons(ETH_P_MAP))
		goto err_drop;

	if (ipa_endpoint_skb_tx(endpoint, skb))
		return NETDEV_TX_BUSY;

	stats->tx_packets++;
	stats->tx_bytes += skb_len;

	return NETDEV_TX_OK;

err_drop:
	dev_kfree_skb_any(skb);
	stats->tx_dropped++;

	return NETDEV_TX_OK;
}

void ipa_netdev_skb_rx(struct net_device *netdev, struct sk_buff *skb)
{
	struct net_device_stats *stats = &netdev->stats;

	if (skb) {
		skb->dev = netdev;
		skb->protocol = htons(ETH_P_MAP);
		stats->rx_packets++;
		stats->rx_bytes += skb->len;

		(void)netif_receive_skb(skb);
	} else {
		stats->rx_dropped++;
	}
}

/** ipa_netdev_ioctl() - I/O control for modem network driver */
static int
ipa_netdev_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct ipa_priv *priv = netdev_priv(netdev);
	struct rmnet_ioctl_extended_s edata = { };
	struct ipa_endpoint *endpoint;
	struct ipa *ipa = priv->ipa;
	void __user *data;

	/* These features are implied; alternatives are not supported */
	if (cmd == RMNET_IOCTL_SET_LLP_IP || cmd == RMNET_IOCTL_OPEN)
		return 0;

	if (cmd != RMNET_IOCTL_EXTENDED)
		return -EINVAL;

	data = ifr->ifr_ifru.ifru_data;

	if (copy_from_user(&edata, data, sizeof(edata)))
		return -EFAULT;

	switch (edata.extended_ioctl) {
	case RMNET_IOCTL_GET_SUPPORTED_FEATURES:	/* Get features */
		edata.u.data = RMNET_IOCTL_FEAT_NOTIFY_MUX_CHANNEL;
		edata.u.data |= RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT;
		edata.u.data |= RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT;
		goto copy_out;

	case RMNET_IOCTL_GET_EPID:			/* Get endpoint ID */
		edata.u.data = 1;
		goto copy_out;

	case RMNET_IOCTL_SET_EGRESS_DATA_FORMAT:	/* Egress data format */
		/* Endpoint is configured for checksum offload enabled */
		if (!(edata.u.data & RMNET_IOCTL_EGRESS_FORMAT_CHECKSUM))
			return -EINVAL;
		/* Endpoint is configured for no (de)aggregation */
		if (edata.u.data & RMNET_IOCTL_EGRESS_FORMAT_AGGREGATION)
			return -EINVAL;

		return 0;

	case RMNET_IOCTL_SET_INGRESS_DATA_FORMAT:	/* Ingress format */
		/* Endpoint is configured for checksum offload enabled */
		if (!(edata.u.data & RMNET_IOCTL_INGRESS_FORMAT_CHECKSUM))
			return -EINVAL;
		/* Endpoint is configured for no aggregation */
		if (edata.u.data & RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA)
			return -EINVAL;

		return 0;

	case RMNET_IOCTL_GET_EP_PAIR:			/* Get endpoint pair */
		endpoint = ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX];
		edata.u.ipa_endpoint_pair.consumer_pipe_num =
				endpoint->endpoint_id;
		endpoint = ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX];
		edata.u.ipa_endpoint_pair.producer_pipe_num =
				endpoint->endpoint_id;
		goto copy_out;

	default:
		return -EINVAL;		/* Invalid (unrecognized) command */
	}

copy_out:
	return copy_to_user(data, &edata, sizeof(edata)) ? -EFAULT : 0;
}

static const struct net_device_ops ipa_netdev_ops = {
	.ndo_open	= ipa_netdev_open,
	.ndo_stop	= ipa_netdev_stop,
	.ndo_start_xmit	= ipa_netdev_xmit,
	.ndo_do_ioctl	= ipa_netdev_ioctl,
};

/** netdev_setup() - netdev setup function  */
static void netdev_setup(struct net_device *netdev)
{
	netdev->netdev_ops = &ipa_netdev_ops;
	ether_setup(netdev);
	/* No header ops (override value set by ether_setup()) */
	netdev->header_ops = NULL;
	netdev->type = ARPHRD_RAWIP;
	netdev->hard_header_len = 0;
	netdev->max_mtu = IPA_MTU;
	netdev->mtu = netdev->max_mtu;
	netdev->addr_len = 0;
	netdev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);
	/* The endpoint is configured for QMAP */
	netdev->needed_headroom = sizeof(struct rmnet_map_header);
	netdev->needed_tailroom = TAILROOM;
	netdev->watchdog_timeo = IPA_NETDEV_TIMEOUT * HZ;
	netdev->hw_features = NETIF_F_SG;
}

/** ipa_netdev_suspend() - suspend callback for runtime_pm
 * @dev: pointer to device
 *
 * This callback will be invoked by the runtime_pm framework when an AP suspend
 * operation is invoked, usually by pressing a suspend button.
 *
 * Returns -EAGAIN to runtime_pm framework in case there are pending packets
 * in the Tx queue. This will postpone the suspend operation until all the
 * pending packets will be transmitted.
 *
 * In case there are no packets to send, releases the WWAN0_PROD entity.
 * As an outcome, the number of IPA active clients should be decremented
 * until IPA clocks can be gated.
 */
void ipa_netdev_suspend(struct net_device *netdev)
{
	struct ipa_priv *priv = netdev_priv(netdev);
	struct ipa *ipa = priv->ipa;

	netif_stop_queue(netdev);

	ipa_endpoint_suspend(ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]);
	ipa_endpoint_suspend(ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]);
}

/** ipa_netdev_resume() - resume callback for runtime_pm
 * @dev: pointer to device
 *
 * This callback will be invoked by the runtime_pm framework when an AP resume
 * operation is invoked.
 *
 * Enables the network interface queue and returns success to the
 * runtime_pm framework.
 */
void ipa_netdev_resume(struct net_device *netdev)
{
	struct ipa_priv *priv = netdev_priv(netdev);
	struct ipa *ipa = priv->ipa;

	ipa_endpoint_resume(ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]);
	ipa_endpoint_resume(ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]);

	netif_wake_queue(netdev);
}

int ipa_netdev_setup(struct ipa *ipa)
{
	struct ipa_endpoint *rx_endpoint;
	struct ipa_endpoint *tx_endpoint;
	struct net_device *netdev;
	struct ipa_priv *priv;
	int ret;

	/* Zero modem shared memory before we begin */
	ret = ipa_mem_zero_modem(ipa);
	if (ret)
		return ret;

	/* Start QMI communication with the modem */
	ret = ipa_qmi_setup(ipa);
	if (ret)
		return ret;

	netdev = alloc_netdev(sizeof(struct ipa_priv), IPA_NETDEV_NAME,
			      NET_NAME_UNKNOWN, netdev_setup);
	if (!netdev) {
		ret = -ENOMEM;
		goto err_qmi_exit;
	}

	rx_endpoint = ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX];
	rx_endpoint->netdev = netdev;

	tx_endpoint = ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX];
	tx_endpoint->netdev = netdev;

	priv = netdev_priv(netdev);
	priv->ipa = ipa;

	ret = register_netdev(netdev);
	if (ret)
		goto err_free_netdev;

	ipa->modem_netdev = netdev;

	return 0;

err_free_netdev:
	free_netdev(netdev);
err_qmi_exit:
	ipa_qmi_teardown(ipa);

	return ret;
}

void ipa_netdev_teardown(struct ipa *ipa)
{
	struct net_device *netdev = ipa->modem_netdev;
	struct ipa_priv *priv;

	if (!netdev)
		return;

	priv = netdev_priv(netdev);
	if (!netif_queue_stopped(netdev))
		(void)ipa_netdev_stop(netdev);

	unregister_netdev(netdev);

	free_netdev(netdev);

	ipa_qmi_teardown(ipa);
}
