/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */
#ifndef _IPA_SMP2P_H_
#define _IPA_SMP2P_H_

#include <linux/types.h>

struct ipa;

/**
 * ipa_smp2p_init() - Initialize the IPA SMP2P subsystem
 * @ipa:	IPA pointer
 * @modem_init:	Whether the modem is responsible for GSI initialization
 *
 * @Return:	Pointer to IPA SMP2P info, or a pointer-coded error
 */
struct ipa_smp2p *ipa_smp2p_init(struct ipa *ipa, bool modem_init);

/**
 * ipa_smp2p_exit() - Inverse of ipa_smp2p_init()
 * @smp2p:	SMP2P information pointer
 */
void ipa_smp2p_exit(struct ipa_smp2p *smp2p);

/**
 * ipa_smp2p_disable() - Prevent "ipa-setup-ready" interrupt handling
 * @smp2p:	SMP2P information pointer
 *
 * Prevent handling of the "setup ready" interrupt from the modem.
 * This is used before initiating shutdown of the driver.
 */
void ipa_smp2p_disable(struct ipa_smp2p *smp2p);

/**
 * ipa_smp2p_notify_reset() - Reset modem notification state
 * @smp2p:	SMP2P information pointer
 *
 * If the modem crashes it queries the IPA clock state.  In cleaning
 * up after such a crash this is used to reset some state maintained
 * for managing this notification.
 */
void ipa_smp2p_notify_reset(struct ipa_smp2p *smp2p);

#endif /* _IPA_SMP2P_H_ */
