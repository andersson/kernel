#ifndef __WCNSS_CTRL_H__
#define __WCNSS_CTRL_H__

#include <linux/rpmsg.h>

struct rpmsg_endpoint *qcom_wcnss_open_channel(void *wcnss, const char *name, rpmsg_rx_cb_t cb, void *priv);

#endif
