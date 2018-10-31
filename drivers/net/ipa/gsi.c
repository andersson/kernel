// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/io.h>
#include <linux/bug.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>

#include "gsi.h"
#include "gsi_reg.h"
#include "gsi_private.h"
#include "gsi_trans.h"
#include "ipa_gsi.h"
#include "ipa_data.h"

/**
 * DOC: The IPA Generic Software Interface
 *
 * The generic software interface (GSI) is an integral component of the IPA,
 * providing a well-defined communication layer between the AP subsystem
 * and the IPA core.  The modem uses the GSI layer as well.
 *
 *	--------	     ---------
 *	|      |	     |	     |
 *	|  AP  +<---.	.----+ Modem |
 *	|      +--. |	| .->+	     |
 *	|      |  | |	| |  |	     |
 *	--------  | |	| |  ---------
 *		  v |	v |
 *		--+-+---+-+--
 *		|    GSI    |
 *		|-----------|
 *		|	    |
 *		|    IPA    |
 *		|	    |
 *		-------------
 *
 * In the above diagram, the AP and Modem represent "execution environments"
 * (EEs), which are independent operating environments that use the IPA for
 * data transfer.
 *
 * Each EE uses a set of unidirectional GSI "channels," which allow transfer
 * of data to or from the IPA.  A channel is implemented as a ring buffer,
 * with a DRAM-resident array of "transfer elements" (TREs) available to
 * describe transfers to or from other EEs through the IPA.  A transfer
 * element can also contain an immediate command, requesting the IPA perform
 * actions other than data transfer.
 *
 * Each TRE refers to a block of data--also located DRAM.  After writing one
 * or more TREs to a channel, the writer (either the IPA or an EE) writes a
 * doorbell register to inform the receiving side how many elements have
 * been written.  Writing to a doorbell register triggers within the GSI.
 *
 * Each channel has a GSI "event ring" associated with it.  An event ring
 * is implemented very much like a channel ring, but is always directed from
 * the IPA to an EE.  The IPA notifies an EE (such as the AP) about channel
 * events by adding an entry to the event ring associated with the channel.
 * The GSI then writes its doorbell for the event ring, causing the target
 * EE to be interrupted.  Each entry in an event ring contains a pointer
 * to the channel TRE whose completion the event represents.
 *
 * Each TRE in a channel ring has a set of flags.  One flag indicates whether
 * the completion of the transfer operation generates an entry (and possibly
 * an interrupt) in the channel's event ring.  Other flags allow transfer
 * elements to be chained together, forming a single logical transaction.
 * TRE flags are used to control whether and when interrupts are generated
 * to signal completion of channel transfers.
 *
 * Elements in channel and event rings are completed (or consumed) strictly
 * in order.  Completion of one entry implies the completion of all preceding
 * entries.  A single completion interrupt can therefore communicate the
 * completion of many transfers.
 *
 * Note that all GSI registers are little-endian, which is the assumed
 * endianness of I/O space accesses.  The accessor functions perform byte
 * swapping if needed (i.e., for a big endian CPU).
 */

/* Delay period for interrupt moderation (in 32KHz IPA internal timer ticks) */
#define IPA_GSI_EVT_RING_INT_MODT	(32 * 1) /* 1ms under 32KHz clock */

#define GSI_CMD_TIMEOUT		5	/* seconds */

#define GSI_MHI_ER_START	10	/* First reserved event number */
#define GSI_MHI_ER_END		16	/* Last reserved event number */

#define GSI_ISR_MAX_ITER	50	/* Detect interrupt storms */

/* Hardware values from the error log register error code field */
enum gsi_err_code {
	GSI_INVALID_TRE_ERR			= 0x1,
	GSI_OUT_OF_BUFFERS_ERR			= 0x2,
	GSI_OUT_OF_RESOURCES_ERR		= 0x3,
	GSI_UNSUPPORTED_INTER_EE_OP_ERR		= 0x4,
	GSI_EVT_RING_EMPTY_ERR			= 0x5,
	GSI_NON_ALLOCATED_EVT_ACCESS_ERR	= 0x6,
	GSI_HWO_1_ERR				= 0x8,
};

/* Hardware values from the error log register error type field */
enum gsi_err_type {
	GSI_ERR_TYPE_GLOB	= 0x1,
	GSI_ERR_TYPE_CHAN	= 0x2,
	GSI_ERR_TYPE_EVT	= 0x3,
};

/* Fields in an error log register at GSI_ERROR_LOG_OFFSET */
#define GSI_LOG_ERR_ARG3_FMASK		GENMASK(3, 0)
#define GSI_LOG_ERR_ARG2_FMASK		GENMASK(7, 4)
#define GSI_LOG_ERR_ARG1_FMASK		GENMASK(11, 8)
#define GSI_LOG_ERR_CODE_FMASK		GENMASK(15, 12)
#define GSI_LOG_ERR_VIRT_IDX_FMASK	GENMASK(23, 19)
#define GSI_LOG_ERR_TYPE_FMASK		GENMASK(27, 24)
#define GSI_LOG_ERR_EE_FMASK		GENMASK(31, 28)

/* Hardware values used when programming an event ring */
enum gsi_evt_chtype {
	GSI_EVT_CHTYPE_MHI_EV	= 0x0,
	GSI_EVT_CHTYPE_XHCI_EV	= 0x1,
	GSI_EVT_CHTYPE_GPI_EV	= 0x2,
	GSI_EVT_CHTYPE_XDCI_EV	= 0x3,
};

/* Hardware values used when programming a channel */
enum gsi_channel_protocol {
	GSI_CHANNEL_PROTOCOL_MHI	= 0x0,
	GSI_CHANNEL_PROTOCOL_XHCI	= 0x1,
	GSI_CHANNEL_PROTOCOL_GPI	= 0x2,
	GSI_CHANNEL_PROTOCOL_XDCI	= 0x3,
};

/* Hardware values representing an event ring immediate command opcode */
enum gsi_evt_cmd_opcode {
	GSI_EVT_ALLOCATE	= 0x0,
	GSI_EVT_RESET		= 0x9,
	GSI_EVT_DE_ALLOC	= 0xa,
};

/* Hardware values representing a generic immediate command opcode */
enum gsi_generic_cmd_opcode {
	GSI_GENERIC_HALT_CHANNEL	= 0x1,
	GSI_GENERIC_ALLOCATE_CHANNEL	= 0x2,
};

/* Hardware values representing a channel immediate command opcode */
enum gsi_ch_cmd_opcode {
	GSI_CH_ALLOCATE	= 0x0,
	GSI_CH_START	= 0x1,
	GSI_CH_STOP	= 0x2,
	GSI_CH_RESET	= 0x9,
	GSI_CH_DE_ALLOC	= 0xa,
	GSI_CH_DB_STOP	= 0xb,
};

/** gsi_gpi_channel_scratch - GPI protocol scratch register
 *
 * @max_outstanding_tre:
 *	Defines the maximum number of TREs allowed in a single transaction
 *	on a channel (in Bytes).  This determines the amount of prefetch
 *	performed by the hardware.  We configure this to equal the size of
 *	the TLV FIFO for the channel.
 * @outstanding_threshold:
 *	Defines the threshold (in Bytes) determining when the sequencer
 *	should update the channel doorbell.  We configure this to equal
 *	the size of two TREs.
 */
struct gsi_gpi_channel_scratch {
	u64 reserved1;
	u16 reserved2;
	u16 max_outstanding_tre;
	u16 reserved3;
	u16 outstanding_threshold;
};

/** gsi_channel_scratch - channel scratch configuration area
 *
 * The exact interpretation of this register is protocol-specific.
 * We only use GPI channels; see struct gsi_gpi_channel_scratch, above.
 */
union gsi_channel_scratch {
	struct gsi_gpi_channel_scratch gpi;
	struct {
		u32 word1;
		u32 word2;
		u32 word3;
		u32 word4;
	} data;
};

/* Return the channel id associated with a given channel */
static u32 gsi_channel_id(struct gsi_channel *channel)
{
	return channel - &channel->gsi->channel[0];
}

/* Report the number of bytes queued to hardware since last call */
void gsi_channel_tx_queued(struct gsi_channel *channel)
{
	u32 trans_count;
	u32 byte_count;

	trans_count = channel->trans_count - channel->queued_trans_count;
	byte_count = channel->byte_count - channel->queued_byte_count;
	channel->queued_trans_count = channel->trans_count;
	channel->queued_byte_count = channel->byte_count;

	ipa_gsi_channel_tx_queued(channel->gsi, gsi_channel_id(channel),
				  trans_count, byte_count);
}

static void gsi_irq_event_enable(struct gsi *gsi, u32 evt_ring_id)
{
	u32 val;

	gsi->event_enable_bitmap |= BIT(evt_ring_id);
	val = gsi->event_enable_bitmap;
	iowrite32(val, gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_MSK_OFFSET);
}

static void gsi_irq_event_disable(struct gsi *gsi, u32 evt_ring_id)
{
	u32 val;

	gsi->event_enable_bitmap &= ~BIT(evt_ring_id);
	val = gsi->event_enable_bitmap;
	iowrite32(val, gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_MSK_OFFSET);
}

/* Enable all GSI_interrupt types */
static void gsi_irq_enable(struct gsi *gsi)
{
	u32 val;

	/* Inter EE commands / interrupt are not supported. */
	val = GSI_CNTXT_TYPE_IRQ_MSK_ALL;
	iowrite32(val, gsi->virt + GSI_CNTXT_TYPE_IRQ_MSK_OFFSET);

	val = GENMASK(gsi->channel_count - 1, 0);
	iowrite32(val, gsi->virt + GSI_CNTXT_SRC_CH_IRQ_MSK_OFFSET);

	val = GENMASK(gsi->evt_ring_count - 1, 0);
	iowrite32(val, gsi->virt + GSI_CNTXT_SRC_EV_CH_IRQ_MSK_OFFSET);

	/* Each IEOB interrupt is enabled (later) as needed by channels */
	iowrite32(0, gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_MSK_OFFSET);

	val = GSI_CNTXT_GLOB_IRQ_ALL;
	iowrite32(val, gsi->virt + GSI_CNTXT_GLOB_IRQ_EN_OFFSET);

	/* Never enable GSI_BREAK_POINT */
	val = GSI_CNTXT_GSI_IRQ_ALL & ~EN_BREAK_POINT_FMASK;
	iowrite32(val, gsi->virt + GSI_CNTXT_GSI_IRQ_EN_OFFSET);
}

/* Disable all GSI_interrupt types */
static void gsi_irq_disable(struct gsi *gsi)
{
	iowrite32(0, gsi->virt + GSI_CNTXT_GSI_IRQ_EN_OFFSET);
	iowrite32(0, gsi->virt + GSI_CNTXT_GLOB_IRQ_EN_OFFSET);
	iowrite32(0, gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_MSK_OFFSET);
	iowrite32(0, gsi->virt + GSI_CNTXT_SRC_EV_CH_IRQ_MSK_OFFSET);
	iowrite32(0, gsi->virt + GSI_CNTXT_SRC_CH_IRQ_MSK_OFFSET);
	iowrite32(0, gsi->virt + GSI_CNTXT_TYPE_IRQ_MSK_OFFSET);
}

/* Return the hardware's notion of the current state of a channel */
static enum gsi_channel_state gsi_channel_state(struct gsi_channel *channel)
{
	u32 channel_id = gsi_channel_id(channel);
	struct gsi *gsi = channel->gsi;
	u32 val;

	val = ioread32(gsi->virt + GSI_CH_C_CNTXT_0_OFFSET(channel_id));

	return u32_get_bits(val, CHSTATE_FMASK);
}

/* Return the hardware's notion of the current state of an event ring */
static enum gsi_evt_ring_state
gsi_evt_ring_state(struct gsi *gsi, u32 evt_ring_id)
{
	u32 val = ioread32(gsi->virt + GSI_EV_CH_E_CNTXT_0_OFFSET(evt_ring_id));

	return u32_get_bits(val, EV_CHSTATE_FMASK);
}

/* Channel control interrupt handler */
static void gsi_isr_chan_ctrl(struct gsi *gsi)
{
	u32 channel_mask;

	channel_mask = ioread32(gsi->virt + GSI_CNTXT_SRC_CH_IRQ_OFFSET);
	iowrite32(channel_mask, gsi->virt + GSI_CNTXT_SRC_CH_IRQ_CLR_OFFSET);

	while (channel_mask) {
		u32 channel_id = __ffs(channel_mask);
		struct gsi_channel *channel;

		channel_mask ^= BIT(channel_id);

		channel = &gsi->channel[channel_id];
		channel->state = gsi_channel_state(channel);

		complete(&channel->completion);
	}
}

static void gsi_isr_evt_ctrl(struct gsi *gsi)
{
	u32 event_mask;

	event_mask = ioread32(gsi->virt + GSI_CNTXT_SRC_EV_CH_IRQ_OFFSET);
	iowrite32(event_mask, gsi->virt + GSI_CNTXT_SRC_EV_CH_IRQ_CLR_OFFSET);

	while (event_mask) {
		u32 evt_ring_id = __ffs(event_mask);
		struct gsi_evt_ring *evt_ring;

		event_mask ^= BIT(evt_ring_id);

		evt_ring = &gsi->evt_ring[evt_ring_id];
		evt_ring->state = gsi_evt_ring_state(gsi, evt_ring_id);

		complete(&evt_ring->completion);
	}
}

static void
gsi_isr_glob_chan_err(struct gsi *gsi, u32 err_ee, u32 channel_id, u32 code)
{
	if (code == GSI_OUT_OF_RESOURCES_ERR) {
		dev_err(gsi->dev, "channel %u out of resources\n", channel_id);
		complete(&gsi->channel[channel_id].completion);
		return;
	}

	/* Report, but otherwise ignore all other error codes */
	dev_err(gsi->dev, "channel %u global error ee 0x%08x code 0x%08x\n",
		channel_id, err_ee, code);
}

static void
gsi_isr_glob_evt_err(struct gsi *gsi, u32 err_ee, u32 evt_ring_id, u32 code)
{
	if (code == GSI_OUT_OF_RESOURCES_ERR) {
		struct gsi_evt_ring *evt_ring = &gsi->evt_ring[evt_ring_id];
		u32 channel_id = gsi_channel_id(evt_ring->channel);

		complete(&evt_ring->completion);
		dev_err(gsi->dev, "evt_ring for channel %u out of resources\n",
			channel_id);
		return;
	}

	/* Report, but otherwise ignore all other error codes */
	dev_err(gsi->dev, "event ring %u global error ee %u code 0x%08x\n",
		evt_ring_id, err_ee, code);
}

static void gsi_isr_glob_err(struct gsi *gsi)
{
	enum gsi_err_type type;
	enum gsi_err_code code;
	u32 which;
	u32 val;
	u32 ee;

	/* Get the logged error, then reinitialize the log */
	val = ioread32(gsi->virt + GSI_ERROR_LOG_OFFSET);
	iowrite32(0, gsi->virt + GSI_ERROR_LOG_OFFSET);
	iowrite32(~0, gsi->virt + GSI_ERROR_LOG_CLR_OFFSET);

	ee = u32_get_bits(val, GSI_LOG_ERR_EE_FMASK);
	which = u32_get_bits(val, GSI_LOG_ERR_VIRT_IDX_FMASK);
	type = u32_get_bits(val, GSI_LOG_ERR_TYPE_FMASK);
	code = u32_get_bits(val, GSI_LOG_ERR_CODE_FMASK);

	if (type == GSI_ERR_TYPE_CHAN)
		gsi_isr_glob_chan_err(gsi, ee, which, code);
	else if (type == GSI_ERR_TYPE_EVT)
		gsi_isr_glob_evt_err(gsi, ee, which, code);
	else	/* type GSI_ERR_TYPE_GLOB should be fatal */
		dev_err(gsi->dev, "unexpected global error 0x%08x\n", type);
}

static void gsi_isr_gp_int1(struct gsi *gsi)
{
	u32 result;
	u32 val;

	val = ioread32(gsi->virt + GSI_CNTXT_SCRATCH_0_OFFSET);
	result = u32_get_bits(val, GENERIC_EE_RESULT_FMASK);
	if (result != GENERIC_EE_SUCCESS_FVAL)
		dev_err(gsi->dev, "global INT1 generic result %u\n", result);

	complete(&gsi->completion);
}

static void gsi_isr_glob_ee(struct gsi *gsi)
{
	u32 val;

	val = ioread32(gsi->virt + GSI_CNTXT_GLOB_IRQ_STTS_OFFSET);

	if (val & ERROR_INT_FMASK)
		gsi_isr_glob_err(gsi);

	iowrite32(val, gsi->virt + GSI_CNTXT_GLOB_IRQ_CLR_OFFSET);

	val &= ~ERROR_INT_FMASK;

	if (val & EN_GP_INT1_FMASK) {
		val ^= EN_GP_INT1_FMASK;
		gsi_isr_gp_int1(gsi);
	}

	if (val)
		dev_err(gsi->dev, "unexpected global interrupt 0x%08x\n", val);
}

/* I/O completion interrupt event */
static void gsi_isr_ieob(struct gsi *gsi)
{
	u32 event_mask;

	event_mask = ioread32(gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_OFFSET);
	iowrite32(event_mask, gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_CLR_OFFSET);

	while (event_mask) {
		u32 evt_ring_id = __ffs(event_mask);

		event_mask ^= BIT(evt_ring_id);

		gsi_irq_event_disable(gsi, evt_ring_id);
		napi_schedule(&gsi->evt_ring[evt_ring_id].channel->napi);
	}
}

/* We don't currently expect to receive any inter-EE channel interrupts */
static void gsi_isr_inter_ee_chan_ctrl(struct gsi *gsi)
{
	u32 channel_mask;

	channel_mask = ioread32(gsi->virt + GSI_INTER_EE_SRC_CH_IRQ_OFFSET);
	iowrite32(channel_mask, gsi->virt + GSI_INTER_EE_SRC_CH_IRQ_CLR_OFFSET);

	while (channel_mask) {
		u32 channel_id = __ffs(channel_mask);

		dev_err(gsi->dev, "ch %u inter-EE interrupt\n", channel_id);
		channel_mask ^= BIT(channel_id);
	}
}

/* We don't currently expect to receive any inter-EE event interrupts */
static void gsi_isr_inter_ee_evt_ctrl(struct gsi *gsi)
{
	u32 event_mask;

	event_mask = ioread32(gsi->virt + GSI_INTER_EE_SRC_EV_CH_IRQ_OFFSET);
	iowrite32(event_mask,
		  gsi->virt + GSI_INTER_EE_SRC_EV_CH_IRQ_CLR_OFFSET);

	while (event_mask) {
		u32 evt_ring_id = __ffs(event_mask);

		event_mask ^= BIT(evt_ring_id);

		/* not currently expected */
		dev_err(gsi->dev, "evt %u inter-EE interrupt\n", evt_ring_id);
	}
}

/* We don't currently expect to receive any general event interrupts */
static void gsi_isr_general(struct gsi *gsi)
{
	u32 val;

	val = ioread32(gsi->virt + GSI_CNTXT_GSI_IRQ_STTS_OFFSET);
	iowrite32(val, gsi->virt + GSI_CNTXT_GSI_IRQ_CLR_OFFSET);

	if (val & CLR_BREAK_POINT_FMASK)
		dev_err(gsi->dev, "breakpoint!\n");
	val ^= CLR_BREAK_POINT_FMASK;

	if (val)
		dev_err(gsi->dev, "unexpected general interrupt 0x%08x\n", val);
}

/**
 * gsi_isr() - Top level GSI interrupt service routine
 * @irq:	Interrupt number (ignored)
 * @dev_id:	GSI pointer supplied to request_irq()
 *
 * This is the main handler function registered for the GSI IRQ. Each type
 * of interrupt has a separate handler function that is called from here.
 */
static irqreturn_t gsi_isr(int irq, void *dev_id)
{
	struct gsi *gsi = dev_id;
	u32 intr_mask;
	u32 cnt = 0;

	while ((intr_mask = ioread32(gsi->virt + GSI_CNTXT_TYPE_IRQ_OFFSET))) {
		/* intr_mask contains bitmask of pending GSI interrupts */
		do {
			u32 gsi_intr = BIT(__ffs(intr_mask));

			intr_mask ^= gsi_intr;

			switch (gsi_intr) {
			case CH_CTRL_FMASK:
				gsi_isr_chan_ctrl(gsi);
				break;
			case EV_CTRL_FMASK:
				gsi_isr_evt_ctrl(gsi);
				break;
			case GLOB_EE_FMASK:
				gsi_isr_glob_ee(gsi);
				break;
			case IEOB_FMASK:
				gsi_isr_ieob(gsi);
				break;
			case INTER_EE_CH_CTRL_FMASK:
				gsi_isr_inter_ee_chan_ctrl(gsi);
				break;
			case INTER_EE_EV_CTRL_FMASK:
				gsi_isr_inter_ee_evt_ctrl(gsi);
				break;
			case GENERAL_FMASK:
				gsi_isr_general(gsi);
				break;
			default:
				dev_err(gsi->dev,
					"%s: unrecognized type 0x%08x\n",
					__func__, gsi_intr);
				break;
			}
		} while (intr_mask);

		if (++cnt > GSI_ISR_MAX_ITER) {
			dev_err(gsi->dev, "interrupt flood\n");
			break;
		}
	}

	return IRQ_HANDLED;
}

/* Return the virtual address associated with a ring index */
void *gsi_ring_virt(struct gsi_ring *ring, u32 index)
{
	/* Note: index *must* be used modulo the ring count here */
	return ring->virt + (index % ring->count) * sizeof(struct gsi_tre);
}

/* Return the 32-bit DMA address associated with a ring index */
u32 gsi_ring_addr(struct gsi_ring *ring, u32 index)
{
	return (ring->addr & GENMASK(31, 0)) + index * sizeof(struct gsi_tre);
}

/* Return the ring index of a 32-bit ring offset */
static u32 gsi_ring_index(struct gsi_ring *ring, u32 offset)
{
	/* Code assumes channel and event ring elements are the same size */
	BUILD_BUG_ON(sizeof(struct gsi_tre) !=
		     sizeof(struct gsi_xfer_compl_evt));

	return (offset - gsi_ring_addr(ring, 0)) / sizeof(struct gsi_tre);
}

/* Return the transaction associated with a transfer completion event */
static struct gsi_trans *gsi_event_trans(struct gsi_channel *channel,
					 struct gsi_xfer_compl_evt *evt)
{
	u32 tre_offset;
	u32 tre_index;

	/* Event xfer_ptr records the TRE it's associated with */
	tre_offset = le64_to_cpu(evt->xfer_ptr) & GENMASK(31, 0);
	tre_index = gsi_ring_index(&channel->tre_ring, tre_offset);

	return gsi_channel_trans_mapped(channel, tre_index);
}
/**
 * gsi_channel_tx_update() - Report completed TX transfers
 * @channel:	Channel that has completed transmitting packets
 * @trans:	Last transation known to be complete
 *
 * Compute the number of transactions and bytes that have been
 * transferred on a TX channel, and report that to higher layers in
 * the network stack for throttling.
 */
static void
gsi_channel_tx_update(struct gsi_channel *channel, struct gsi_trans *trans)
{
	u64 byte_count = trans->byte_count + trans->len;
	u64 trans_count = trans->trans_count + 1;

	byte_count -= channel->compl_byte_count;
	channel->compl_byte_count += byte_count;
	trans_count -= channel->compl_trans_count;
	channel->compl_trans_count += trans_count;;
	/* assert(trans_count <= U32_MAX); */

	ipa_gsi_channel_tx_completed(channel->gsi, gsi_channel_id(channel),
				     trans_count, byte_count);
}

/**
 * gsi_evt_ring_rx_update() - Record lengths of received data
 * @evt_ring:	Event ring associated with channel that received packets
 * @index:	Event index in ring reported by hardware
 *
 * Events for RX channels contain the actual number of bytes received into
 * the buffer.  Every event has a transaction associated with it, and here
 * we update transactions to record their actual received lengths.
 *
 * This function is called whenever we learn that the GSI hardware has filled
 * new events since the last time we checked.  The ring's index field tells
 * the first entry in need of processing.  The index provided is the
 * first *unfilled* event in the ring (following the last filled one).
 *
 * Events are sequential within the event ring, and transactions are
 * sequential within the transaction pool.
 *
 * Note that @index always refers to an element *within* the event ring.
 */
static void gsi_evt_ring_rx_update(struct gsi_evt_ring *evt_ring, u32 index)
{
	struct gsi_channel *channel = evt_ring->channel;
	struct gsi_ring *ring = &evt_ring->ring;
	struct gsi_xfer_compl_evt *evt_done;
	struct gsi_trans_info *trans_info;
	struct gsi_xfer_compl_evt *evt;
	struct gsi_trans *trans;
	u32 byte_count = 0;
	u32 trans_avail;
	u32 old_index;
	u32 evt_avail;

	/* We'll start with the oldest un-processed event.  RX channels
	 * replenish receive buffers in single-TRE transactions, so we
	 * can just map that event to its transaction.
	 */
	old_index = ring->index;
	evt = gsi_ring_virt(ring, old_index);
	trans = gsi_event_trans(channel, evt);

	/* Compute the number of events to process before we wrap */
	evt_avail = ring->count - old_index % ring->count;

	/* And compute how many transactions to process before we wrap */
	trans_info = &channel->trans_info;
	trans_avail = (u32)(&trans_info->pool[trans_info->pool_count] - trans);

	/* Finally, determine when we'll be done processing events */
	evt_done = gsi_ring_virt(ring, index);
	do {
		trans->len = __le16_to_cpu(evt->len);
		byte_count += trans->result;

		if (--evt_avail)
			evt++;
		else
			evt = gsi_ring_virt(ring, 0);

		if (--trans_avail)
			trans++;
		else
			trans = &trans_info->pool[0];
	} while (evt != evt_done);

	/* We record RX bytes when they are received */
	channel->byte_count += byte_count;
	channel->trans_count++;
}

/* Ring an event ring doorbell, reporting the last entry processed by the AP.
 * The index argument (modulo the ring count) is the first unfilled entry, so
 * we supply one less than that with the doorbell.  Update the event ring
 * index field with the value provided.
 */
static void gsi_evt_ring_doorbell(struct gsi *gsi, u32 evt_ring_id, u32 index)
{
	struct gsi_ring *ring = &gsi->evt_ring[evt_ring_id].ring;
	u32 val;

	ring->index = index;	/* Next unused entry */

	/* Note: index *must* be used modulo the ring count here */
	val = gsi_ring_addr(ring, (index - 1) % ring->count);
	iowrite32(val, gsi->virt + GSI_EV_CH_E_DOORBELL_0_OFFSET(evt_ring_id));
}

/* Issue a GSI command by writing a value to a register, then wait for
 * completion to be signaled.  Reports an error if the command times out.
 * (Timeout is not expected, and suggests broken hardware.)
 */
static void
gsi_command(struct gsi *gsi, u32 reg, u32 val, struct completion *completion)
{
	reinit_completion(completion);

	iowrite32(val, gsi->virt + reg);
	if (!wait_for_completion_timeout(completion, GSI_CMD_TIMEOUT * HZ))
		dev_err(gsi->dev, "%s timeout reg 0x%08x val 0x%08x\n",
			__func__, reg, val);
}

/* Issue an event ring command and wait for it to complete */
static void evt_ring_command(struct gsi *gsi, u32 evt_ring_id,
			     enum gsi_evt_cmd_opcode op)
{
	struct completion *completion = &gsi->evt_ring[evt_ring_id].completion;
	u32 val = 0;

	val |= u32_encode_bits(evt_ring_id, EV_CHID_FMASK);
	val |= u32_encode_bits(op, EV_OPCODE_FMASK);

	gsi_command(gsi, GSI_EV_CH_CMD_OFFSET, val, completion);
}

static void
gsi_modem_channel_common(struct gsi *gsi, u32 channel_id, bool alloc)
{
	struct completion *completion = &gsi->completion;
	u32 opcode;
	u32 val;

	opcode = alloc ? GSI_GENERIC_ALLOCATE_CHANNEL
		       : GSI_GENERIC_HALT_CHANNEL;
	val = u32_encode_bits(opcode, GENERIC_OPCODE_FMASK);
	val |= u32_encode_bits(channel_id, GENERIC_CHID_FMASK);
	val |= u32_encode_bits(GSI_EE_MODEM, GENERIC_EE_FMASK);

	gsi_command(gsi, GSI_GENERIC_CMD_OFFSET, val, completion);
}

static void gsi_modem_channel_halt(struct gsi *gsi, u32 channel_id)
{
	gsi_modem_channel_common(gsi, channel_id, false);
}

static void gsi_modem_channel_alloc(struct gsi *gsi, u32 channel_id)
{
	gsi_modem_channel_common(gsi, channel_id, true);
}

/* Issue a channel command and wait for it to complete */
static void
gsi_channel_command(struct gsi_channel *channel, enum gsi_ch_cmd_opcode op)
{
	u32 channel_id = gsi_channel_id(channel);
	u32 val = 0;

	val |= u32_encode_bits(channel_id, CH_CHID_FMASK);
	val |= u32_encode_bits(op, CH_OPCODE_FMASK);

	gsi_command(channel->gsi, GSI_CH_CMD_OFFSET, val, &channel->completion);
}

/* Initialize a ring, including allocating DMA memory for its entries */
static int gsi_ring_alloc(struct gsi *gsi, struct gsi_ring *ring, u32 count)
{
	size_t size = count * sizeof(struct gsi_tre);
	dma_addr_t addr;

	BUILD_BUG_ON(!is_power_of_2(sizeof(struct gsi_tre)));

	if (!count)
		return -EINVAL;

	/* Hardware requires a 2^n ring size, with alignment equal to size */
	ring->virt = dma_alloc_coherent(gsi->dev, size, &addr, GFP_KERNEL);
	if (ring->virt && addr % size) {
		dma_free_coherent(gsi->dev, size, ring->virt, ring->addr);
		dev_err(gsi->dev, "unable to alloc 0x%zx-aligned ring buffer\n",
				size);
		return -EINVAL;	/* Not a good error value, but distinct */
	} else if (!ring->virt) {
		return -ENOMEM;
	}
	ring->addr = addr;
	ring->count = count;

	return 0;
}

/* Free a previously-allocated ring */
static void gsi_ring_free(struct gsi *gsi, struct gsi_ring *ring)
{
	size_t size = ring->count * sizeof(struct gsi_tre);

	dma_free_coherent(gsi->dev, size, ring->virt, ring->addr);
}

/* Program an event ring for use */
static void gsi_evt_ring_program(struct gsi *gsi, u32 evt_ring_id)
{
	struct gsi_evt_ring *evt_ring = &gsi->evt_ring[evt_ring_id];
	size_t size = evt_ring->ring.count * sizeof(struct gsi_tre);
	u32 val = 0;

	BUILD_BUG_ON(sizeof(struct gsi_xfer_compl_evt) >
		     field_max(EV_ELEMENT_SIZE_FMASK));

	val |= u32_encode_bits(GSI_EVT_CHTYPE_GPI_EV, EV_CHTYPE_FMASK);
	val |= EV_INTYPE_FMASK;
	val |= u32_encode_bits(sizeof(struct gsi_xfer_compl_evt),
			       EV_ELEMENT_SIZE_FMASK);
	iowrite32(val, gsi->virt + GSI_EV_CH_E_CNTXT_0_OFFSET(evt_ring_id));

	val = u32_encode_bits(size, EV_R_LENGTH_FMASK);
	iowrite32(val, gsi->virt + GSI_EV_CH_E_CNTXT_1_OFFSET(evt_ring_id));

	/* The context 2 and 3 registers store the low-order and
	 * high-order 32 bits of the address of the event ring,
	 * respectively.
	 */
	val = evt_ring->ring.addr & GENMASK(31, 0);
	iowrite32(val, gsi->virt + GSI_EV_CH_E_CNTXT_2_OFFSET(evt_ring_id));

	val = evt_ring->ring.addr >> 32;
	iowrite32(val, gsi->virt + GSI_EV_CH_E_CNTXT_3_OFFSET(evt_ring_id));

	/* Enable interrupt moderation by setting the moderation delay */
	val = u32_encode_bits(IPA_GSI_EVT_RING_INT_MODT, MODT_FMASK);
	val |= u32_encode_bits(1, MODC_FMASK);	/* comes from channel */
	iowrite32(val, gsi->virt + GSI_EV_CH_E_CNTXT_8_OFFSET(evt_ring_id));

	/* No MSI write data, and MSI address high and low address is 0 */
	iowrite32(0, gsi->virt + GSI_EV_CH_E_CNTXT_9_OFFSET(evt_ring_id));
	iowrite32(0, gsi->virt + GSI_EV_CH_E_CNTXT_10_OFFSET(evt_ring_id));
	iowrite32(0, gsi->virt + GSI_EV_CH_E_CNTXT_11_OFFSET(evt_ring_id));

	/* We don't need to get event read pointer updates */
	iowrite32(0, gsi->virt + GSI_EV_CH_E_CNTXT_12_OFFSET(evt_ring_id));
	iowrite32(0, gsi->virt + GSI_EV_CH_E_CNTXT_13_OFFSET(evt_ring_id));
}

/* Issue an allocation request to the hardware for an event ring */
static int gsi_evt_ring_alloc_hw(struct gsi *gsi, u32 evt_ring_id)
{
	struct gsi_evt_ring *evt_ring = &gsi->evt_ring[evt_ring_id];

	evt_ring_command(gsi, evt_ring_id, GSI_EVT_ALLOCATE);

	if (evt_ring->state != GSI_EVT_RING_STATE_ALLOCATED) {
		dev_err(gsi->dev, "evt_ring_id %u allocation bad state %u\n",
			evt_ring_id, evt_ring->state);
		return -EIO;
	}

	gsi_evt_ring_program(gsi, evt_ring_id);

	/* Have the first event in the ring be the first one filled. */
	gsi_evt_ring_doorbell(gsi, evt_ring_id, 0);

	return 0;
}

/* Issue a hardware de-allocation request for an (allocated) event ring */
static void gsi_evt_ring_free_hw(struct gsi *gsi, u32 evt_ring_id)
{
	evt_ring_command(gsi, evt_ring_id, GSI_EVT_RESET);

	evt_ring_command(gsi, evt_ring_id, GSI_EVT_DE_ALLOC);
}

/* Allocate an available event ring id */
static int gsi_evt_ring_id_alloc(struct gsi *gsi)
{
	u32 evt_ring_id;

	if (gsi->event_bitmap == ~0U)
		return -ENOSPC;

	evt_ring_id = ffz(gsi->event_bitmap);
	gsi->event_bitmap |= BIT(evt_ring_id);

	return (int)evt_ring_id;
}

/* Free a previously-allocated event ring id */
static void gsi_evt_ring_id_free(struct gsi *gsi, u32 evt_ring_id)
{
	gsi->event_bitmap &= ~BIT(evt_ring_id);
}

/* Ring a channel doorbell, reporting the first un-filled entry */
void gsi_channel_doorbell(struct gsi_channel *channel)
{
	struct gsi_ring *tre_ring = &channel->tre_ring;
	u32 channel_id = gsi_channel_id(channel);
	struct gsi *gsi = channel->gsi;
	u32 val;

	/* Note: index *must* be used modulo the ring count here */
	val = gsi_ring_addr(tre_ring, tre_ring->index % tre_ring->count);
	iowrite32(val, gsi->virt + GSI_CH_C_DOORBELL_0_OFFSET(channel_id));
}

/* Consult hardware, move any newly completed transactions to completed list */
static void gsi_channel_update(struct gsi_channel *channel)
{
	u32 evt_ring_id = channel->evt_ring_id;
	struct gsi *gsi = channel->gsi;
	struct gsi_evt_ring *evt_ring;
	struct gsi_trans *trans;
	struct gsi_ring *ring;
	u32 offset;
	u32 index;

	evt_ring = &gsi->evt_ring[evt_ring_id];
	ring = &evt_ring->ring;

	/* See if there's anything new to process; if not, we're done.  Note
	 * that index always refers to an entry *within* the event ring.
	 */
	offset = GSI_EV_CH_E_CNTXT_4_OFFSET(evt_ring_id);
	index = gsi_ring_index(ring, ioread32(gsi->virt + offset));
	if (index == ring->index % ring->count)
		return;

	/* Get the transaction for the latest completed event.  Take a
	 * reference to keep it from completing before we give the events
	 * for this and previous transactions back to the hardware.
	 */
	trans = gsi_event_trans(channel, gsi_ring_virt(ring, index - 1));
	refcount_inc(&trans->refcount);

	/* For RX channels, update each completed transaction with the number
	 * of bytes that were actually received.  For TX channels, report
	 * the number of transactions and bytes this completion represents
	 * up the network stack.
	 */
	if (channel->toward_ipa)
		gsi_channel_tx_update(channel, trans);
	else
		gsi_evt_ring_rx_update(evt_ring, index);

	gsi_trans_move_complete(trans);

	/* Tell the hardware we've handled these events */
	gsi_evt_ring_doorbell(channel->gsi, channel->evt_ring_id, index);

	gsi_trans_free(trans);
}

/**
 * gsi_channel_poll_one() - Return a single completed transaction on a channel
 * @channel:	Channel to be polled
 *
 * @Return:	 Transaction pointer, or null if none are available
 *
 * This function returns the first entry on a channel's completed transaction
 * list.  If that list is empty, the hardware is consulted to determine
 * whether any new transactions have completed.  If so, they're moved to the
 * completed list and the new first entry is returned.  If there are no more
 * completed transactions, a null pointer is returned.
 */
static struct gsi_trans *gsi_channel_poll_one(struct gsi_channel *channel)
{
	struct gsi_trans *trans;

	/* Get the first transaction from the completed list */
	trans = gsi_channel_trans_complete(channel);
	if (!trans) {
		/* List is empty; see if there's more to do */
		gsi_channel_update(channel);
		trans = gsi_channel_trans_complete(channel);
	}

	if (trans)
		gsi_trans_move_polled(trans);

	return trans;
}

/**
 * gsi_channel_poll() - NAPI poll function for a channel
 * @napi:	NAPI structure for the channel
 * @budget:	Budget supplied by NAPI core

 * @Return:	 Number of items polled (<= budget)
 *
 * Single transactions completed by hardware are polled until either
 * the budget is exhausted, or there are no more.  Each transaction
 * polled is passed to gsi_trans_complete(), to perform remaining
 * completion processing and retire/free the transaction.
 */
static int gsi_channel_poll(struct napi_struct *napi, int budget)
{
	struct gsi_channel *channel;
	int count = 0;

	channel = container_of(napi, struct gsi_channel, napi);
	while (count < budget) {
		struct gsi_trans *trans;

		trans = gsi_channel_poll_one(channel);
		if (!trans)
			break;
		gsi_trans_complete(trans);
	}

	if (count < budget) {
		napi_complete(&channel->napi);
		gsi_irq_event_enable(channel->gsi, channel->evt_ring_id);
	}

	return count;
}

/* The event bitmap represents which event ids are available for allocation.
 * Set bits are not available, clear bits can be used.  This function
 * initializes the map so all events supported by the hardware are available,
 * then precludes any reserved events from being allocated.
 */
static u32 gsi_event_bitmap_init(u32 evt_ring_max)
{
	u32 event_bitmap = GENMASK(BITS_PER_LONG - 1, evt_ring_max);

	return event_bitmap | GENMASK(GSI_MHI_ER_END, GSI_MHI_ER_START);
}

/* Setup function for event rings */
static int gsi_evt_ring_setup(struct gsi *gsi)
{
	u32 evt_ring_id = 0;

	while (evt_ring_id < gsi->evt_ring_count) {
		struct gsi_evt_ring *evt_ring = &gsi->evt_ring[evt_ring_id];

		evt_ring->state = gsi_evt_ring_state(gsi, evt_ring_id);
		if (evt_ring->state != GSI_EVT_RING_STATE_NOT_ALLOCATED)
			return -EIO;
		evt_ring_id++;
	}

	return 0;
}

/* Inverse of gsi_evt_ring_setup() */
static void gsi_evt_ring_teardown(struct gsi *gsi)
{
	/* Nothing to do */
}

/* Configure a channel's "scratch registers" for a particular protocol */
static void gsi_channel_scratch_write(struct gsi_channel *channel)
{
	u32 channel_id = gsi_channel_id(channel);
	union gsi_channel_scratch scr = { };
	struct gsi_gpi_channel_scratch *gpi;
	struct gsi *gsi = channel->gsi;
	u32 val;

	/* See comments above definition of gsi_gpi_channel_scratch */
	gpi = &scr.gpi;
	gpi->max_outstanding_tre = channel->data->tlv_count *
					sizeof(struct gsi_tre);
	gpi->outstanding_threshold = 2 * sizeof(struct gsi_tre);

	val = scr.data.word1;
	iowrite32(val, gsi->virt + GSI_CH_C_SCRATCH_0_OFFSET(channel_id));

	val = scr.data.word2;
	iowrite32(val, gsi->virt + GSI_CH_C_SCRATCH_1_OFFSET(channel_id));

	val = scr.data.word3;
	iowrite32(val, gsi->virt + GSI_CH_C_SCRATCH_2_OFFSET(channel_id));

	/* We must preserve the upper 16 bits of the last scratch register.
	 * The next sequence assumes those bits remain unchanged between the
	 * read and the write.
	 */
	val = ioread32(gsi->virt + GSI_CH_C_SCRATCH_3_OFFSET(channel_id));
	val = (scr.data.word4 & GENMASK(31, 16)) | (val & GENMASK(15, 0));
	iowrite32(val, gsi->virt + GSI_CH_C_SCRATCH_3_OFFSET(channel_id));
}

/* Program a channel for use */
static void gsi_channel_program(struct gsi_channel *channel, bool doorbell)
{
	size_t size = channel->tre_ring.count * sizeof(struct gsi_tre);
	u32 channel_id = gsi_channel_id(channel);
	struct gsi *gsi = channel->gsi;
	u32 wrr_weight = 0;
	u32 val = 0;

	BUILD_BUG_ON(sizeof(struct gsi_tre) > field_max(ELEMENT_SIZE_FMASK));

	val |= u32_encode_bits(GSI_CHANNEL_PROTOCOL_GPI, CHTYPE_PROTOCOL_FMASK);
	if (channel->toward_ipa)
		val |= CHTYPE_DIR_FMASK;
	val |= u32_encode_bits(channel->evt_ring_id, ERINDEX_FMASK);
	val |= u32_encode_bits(sizeof(struct gsi_tre), ELEMENT_SIZE_FMASK);
	iowrite32(val, gsi->virt + GSI_CH_C_CNTXT_0_OFFSET(channel_id));

	val = u32_encode_bits(size, R_LENGTH_FMASK);
	iowrite32(val, gsi->virt + GSI_CH_C_CNTXT_1_OFFSET(channel_id));

	/* The context 2 and 3 registers store the low-order and
	 * high-order 32 bits of the address of the channel ring,
	 * respectively.
	 */
	val = channel->tre_ring.addr & GENMASK(31, 0);
	iowrite32(val, gsi->virt + GSI_CH_C_CNTXT_2_OFFSET(channel_id));

	val = channel->tre_ring.addr >> 32;
	iowrite32(val, gsi->virt + GSI_CH_C_CNTXT_3_OFFSET(channel_id));

	if (channel->data->wrr_priority)
		wrr_weight = field_max(WRR_WEIGHT_FMASK);
	val = u32_encode_bits(wrr_weight, WRR_WEIGHT_FMASK);

	/* Max prefetch is 1 segment (do not set MAX_PREFETCH_FMASK) */
	if (doorbell)
		val |= USE_DB_ENG_FMASK;

	if (!channel->use_prefetch)
		val |= USE_ESCAPE_BUF_ONLY_FMASK;

	iowrite32(val, gsi->virt + GSI_CH_C_QOS_OFFSET(channel_id));
}

/* Configure a channel; we configure all channels to use GPI protocol */
static void gsi_channel_config(struct gsi_channel *channel, bool db_enable)
{
	/* Start at the first TRE entry each time we configure the channel */
	channel->tre_ring.index = 0;
	gsi_channel_program(channel, db_enable);
	gsi_channel_scratch_write(channel);
}

/* Setup function for a single channel */
static int gsi_channel_setup_one(struct gsi_channel *channel, bool db_enable)
{
	u32 evt_ring_id = channel->evt_ring_id;
	struct gsi *gsi = channel->gsi;
	u32 val;
	int ret;

	if (!gsi)
		return 0;	/* Ignore uninitialized channels */

	channel->state = gsi_channel_state(channel);
	if (channel->state != GSI_CHANNEL_STATE_NOT_ALLOCATED)
		return -EIO;

	mutex_lock(&gsi->mutex);

	ret = gsi_evt_ring_alloc_hw(gsi, evt_ring_id);
	if (ret)
		goto err_mutex_unlock;

	gsi_channel_command(channel, GSI_CH_ALLOCATE);
	gsi->channel_stats.allocate++;

	ret = channel->state == GSI_CHANNEL_STATE_ALLOCATED ? 0 : -EIO;
	if (ret)
		goto err_free_evt_ring;

	gsi_channel_config(channel, db_enable);

	mutex_unlock(&gsi->mutex);

	if (channel->toward_ipa)
		netif_tx_napi_add(&gsi->dummy_dev, &channel->napi,
				  gsi_channel_poll, NAPI_POLL_WEIGHT);
	else
		netif_napi_add(&gsi->dummy_dev, &channel->napi,
			       gsi_channel_poll, NAPI_POLL_WEIGHT);

	/* Enable the event interrupt (clear it first in case pending) */
	val = BIT(evt_ring_id);
	iowrite32(val, gsi->virt + GSI_CNTXT_SRC_IEOB_IRQ_CLR_OFFSET);
	gsi_irq_event_enable(gsi, evt_ring_id);

	return 0;

err_free_evt_ring:
	gsi_evt_ring_free_hw(gsi, evt_ring_id);
err_mutex_unlock:
	mutex_unlock(&gsi->mutex);

	return ret;
}

/* Inverse of gsi_channel_setup_one() */
static void gsi_channel_teardown_one(struct gsi_channel *channel)
{
	u32 evt_ring_id = channel->evt_ring_id;
	struct gsi *gsi = channel->gsi;

	if (!gsi)
		return;

	gsi_irq_event_disable(gsi, evt_ring_id);

	netif_napi_del(&channel->napi);

	mutex_lock(&gsi->mutex);

	gsi_channel_command(channel, GSI_CH_DE_ALLOC);
	gsi->channel_stats.free++;

	gsi_evt_ring_free_hw(gsi, evt_ring_id);

	mutex_unlock(&gsi->mutex);

	gsi_channel_trans_exit(channel);
}

/* Setup function for channels */
static int gsi_channel_setup(struct gsi *gsi, bool db_enable)
{
	u32 channel_id;
	u32 mask;
	int ret;

	ret = gsi_evt_ring_setup(gsi);
	if (ret)
		return ret;

	gsi_irq_enable(gsi);

	for (channel_id = 0; channel_id < gsi->channel_count; channel_id++) {
		struct gsi_channel *channel;

		channel = &gsi->channel[channel_id];
		ret = gsi_channel_setup_one(channel, db_enable);
		if (ret)
			goto err_unwind;
	}

	/* Allocate modem channels if necessary */
	mask = gsi->modem_channel_bitmap;
	while (mask) {
		u32 channel_id = __ffs(mask);

		mask ^= BIT(channel_id);

		gsi_modem_channel_alloc(gsi, channel_id);
	}

	return 0;

err_unwind:
	while (channel_id--)
		gsi_channel_teardown_one(&gsi->channel[channel_id]);
	gsi_irq_disable(gsi);
	gsi_evt_ring_teardown(gsi);

	return ret;
}

/* Inverse of gsi_channel_setup() */
static void gsi_channel_teardown(struct gsi *gsi)
{
	u32 mask = gsi->modem_channel_bitmap;
	u32 channel_id;

	while (mask) {
		u32 channel_id = __fls(mask);

		mask ^= BIT(channel_id);

		gsi_modem_channel_halt(gsi, channel_id);
	}

	channel_id = gsi->channel_count - 1;
	do
		gsi_channel_teardown_one(&gsi->channel[channel_id]);
	while (channel_id--);

	gsi_irq_disable(gsi);
	gsi_evt_ring_teardown(gsi);
}

/* Setup function for GSI.  GSI firmware must be loaded and initialized */
int gsi_setup(struct gsi *gsi, bool db_enable)
{
	u32 val;

	/* Here is where we first touch the GSI hardware */
	val = ioread32(gsi->virt + GSI_GSI_STATUS_OFFSET);
	if (!(val & ENABLED_FMASK)) {
		dev_err(gsi->dev, "GSI has not been enabled\n");
		return -EIO;
	}

	val = ioread32(gsi->virt + GSI_GSI_HW_PARAM_2_OFFSET);

	gsi->channel_count = u32_get_bits(val, NUM_CH_PER_EE_FMASK);
	dev_dbg(gsi->dev, "channel count %u\n", gsi->channel_count);
	if (!gsi->channel_count || gsi->channel_count > GSI_CHANNEL_MAX)
		return -EINVAL;

	gsi->evt_ring_count = u32_get_bits(val, NUM_EV_PER_EE_FMASK);
	dev_dbg(gsi->dev, "evt_ring count %u\n", gsi->evt_ring_count);
	if (!gsi->evt_ring_count || gsi->evt_ring_count > GSI_EVT_RING_MAX)
		return -EINVAL;

	/* Initialize the error log */
	iowrite32(0, gsi->virt + GSI_ERROR_LOG_OFFSET);

	/* Writing 1 indicates IRQ interrupts; 0 would be MSI */
	iowrite32(1, gsi->virt + GSI_CNTXT_INTSET_OFFSET);

	return gsi_channel_setup(gsi, db_enable);
}

/* Inverse of gsi_setup() */
void gsi_teardown(struct gsi *gsi)
{
	gsi_channel_teardown(gsi);
}

/* Initialize a channel's event ring */
static int gsi_channel_evt_ring_init(struct gsi_channel *channel)
{
	struct gsi *gsi = channel->gsi;
	struct gsi_evt_ring *evt_ring;
	int ret;

	ret = gsi_evt_ring_id_alloc(gsi);
	if (ret < 0)
		return ret;
	channel->evt_ring_id = ret;

	evt_ring = &gsi->evt_ring[channel->evt_ring_id];
	evt_ring->channel = channel;

	ret = gsi_ring_alloc(gsi, &evt_ring->ring, channel->data->event_count);
	if (ret)
		goto err_free_evt_ring_id;

	return 0;

err_free_evt_ring_id:
	gsi_evt_ring_id_free(gsi, channel->evt_ring_id);

	return ret;
}

/* Inverse of gsi_channel_evt_ring_init() */
static void gsi_channel_evt_ring_exit(struct gsi_channel *channel)
{
	struct gsi *gsi = channel->gsi;
	struct gsi_evt_ring *evt_ring;

	evt_ring = &gsi->evt_ring[channel->evt_ring_id];
	gsi_ring_free(gsi, &evt_ring->ring);
	gsi_evt_ring_id_free(gsi, channel->evt_ring_id);
}

/* Init function for event rings */
static void gsi_evt_ring_init(struct gsi *gsi)
{
	u32 evt_ring_id;

	BUILD_BUG_ON(GSI_EVT_RING_MAX >= BITS_PER_LONG);

	gsi->event_bitmap = gsi_event_bitmap_init(GSI_EVT_RING_MAX);
	gsi->event_enable_bitmap = 0;
	for (evt_ring_id = 0; evt_ring_id < GSI_EVT_RING_MAX; evt_ring_id++)
		init_completion(&gsi->evt_ring[evt_ring_id].completion);
}

/* Inverse of gsi_evt_ring_init() */
static void gsi_evt_ring_exit(struct gsi *gsi)
{
	/* Nothing to do */
}

/* Init function for a single channel */
static int gsi_channel_init_one(struct gsi *gsi,
				const struct ipa_gsi_endpoint_data *data,
				bool use_prefetch)
{
	struct gsi_channel *channel;
	int ret;

	if (data->ee_id != GSI_EE_AP)
		return 0;	/* Ignore non-AP channels */

	if (data->channel_id >= GSI_CHANNEL_MAX) {
		dev_err(gsi->dev, "bad channel id %u (must be less than %u)\n",
			data->channel_id, GSI_CHANNEL_MAX);
		return -EINVAL;
	}

	/* The value 256 here is arbitrary, and much higher than expected */
	if (!data->channel.tlv_count || data->channel.tlv_count > 256) {
		dev_err(gsi->dev, "bad tlv_count %u (must be 1..256)\n",
			data->channel.tlv_count);
		return -EINVAL;
	}

	if (!is_power_of_2(data->channel.tre_count)) {
		dev_err(gsi->dev, "bad tre_count %u (must be power of 2)\n",
			data->channel.tre_count);
		return -EINVAL;
	}

	if (!is_power_of_2(data->channel.event_count)) {
		dev_err(gsi->dev, "bad event_count %u (must be power of 2)\n",
			data->channel.event_count);
		return -EINVAL;
	}

	channel = &gsi->channel[data->channel_id];
	memset(channel, 0, sizeof(*channel));

	channel->gsi = gsi;
	channel->toward_ipa = data->toward_ipa;
	channel->data = &data->channel;
	channel->use_prefetch = use_prefetch;

	init_completion(&channel->completion);

	ret = gsi_channel_evt_ring_init(channel);
	if (ret)
		return ret;

	ret = gsi_ring_alloc(gsi, &channel->tre_ring, channel->data->tre_count);
	if (ret)
		goto err_channel_evt_ring_exit;

	ret = gsi_channel_trans_init(channel);
	if (ret)
		goto err_ring_free;

	return 0;

err_ring_free:
	gsi_ring_free(gsi, &channel->tre_ring);
err_channel_evt_ring_exit:
	gsi_channel_evt_ring_exit(channel);

	return ret;
}

/* Inverse of gsi_channel_init_one() */
static void gsi_channel_exit_one(struct gsi_channel *channel)
{
	gsi_channel_trans_exit(channel);
	gsi_ring_free(channel->gsi, &channel->tre_ring);
	gsi_channel_evt_ring_exit(channel);
}

/* Init function for channels */
static int gsi_channel_init(struct gsi *gsi, bool prefetch_flag, u32 count,
			    const struct ipa_gsi_endpoint_data *data,
			    bool modem_alloc)
{
	int ret = 0;
	u32 i;

	gsi_evt_ring_init(gsi);

	for (i = 0; i < count; i++) {
		bool use_prefetch;

		if (ipa_gsi_endpoint_data_empty(&data[i]))
			continue;	/* Skip over empty slots */

		/* Mark modem channels to be allocated (hardware workaround) */
		if (modem_alloc && data[i].ee_id == GSI_EE_MODEM) {
			gsi->modem_channel_bitmap |= BIT(data[i].channel_id);
			continue;
		}

		/* The endpoint data array is indexed by endpoint name */
		use_prefetch = prefetch_flag && i == IPA_ENDPOINT_AP_COMMAND_TX;
		ret = gsi_channel_init_one(gsi, &data[i], use_prefetch);
		if (ret)
			break;
	}

	return ret;
}

/* Inverse of gsi_channel_init() */
static void gsi_channel_exit(struct gsi *gsi)
{
	u32 channel_id = GSI_CHANNEL_MAX - 1;

	do
		gsi_channel_exit_one(&gsi->channel[channel_id]);
	while (channel_id--);

	gsi_evt_ring_exit(gsi);
}

/* Init function for GSI.  GSI hardware does not need to be "ready" */
int gsi_init(struct gsi *gsi, struct platform_device *pdev, bool prefetch_flag,
	     u32 count, const struct ipa_gsi_endpoint_data *data,
	     bool modem_alloc)
{
	struct resource *res;
	resource_size_t size;
	unsigned int irq;
	int ret;

	gsi->dev = &pdev->dev;

	/* The GSI layer performs NAPI on all endpoints.  NAPI requires a
	 * network device structure, but the GSI layer does not have one,
	 * so we must create a dummy network device for this purpose.
	 */
	init_dummy_netdev(&gsi->dummy_dev);

	/* Get GSI memory range and map it */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gsi");
	if (!res)
		return -ENXIO;

	size = resource_size(res);
	if (res->start > U32_MAX || size > U32_MAX - res->start)
		return -EINVAL;

	gsi->virt = ioremap(res->start, size);
	if (!gsi->virt)
		return -ENOMEM;

	mutex_init(&gsi->mutex);

	ret = platform_get_irq_byname(pdev, "gsi");
	if (ret < 0)
		goto err_unmap_virt;
	irq = ret;

	ret = request_irq(irq, gsi_isr, 0, "gsi", gsi);
	if (ret)
		goto err_unmap_virt;
	gsi->irq = irq;

	ret = enable_irq_wake(gsi->irq);
	if (ret)
		dev_err(gsi->dev, "error %d enabling gsi wake irq\n", ret);
	gsi->irq_wake_enabled = ret ? 0 : 1;

	ret = gsi_channel_init(gsi, prefetch_flag, count, data, modem_alloc);
	if (ret)
		goto err_mutex_destroy;
	init_completion(&gsi->completion);

	return 0;

err_mutex_destroy:
	if (gsi->irq_wake_enabled)
		(void)disable_irq_wake(gsi->irq);
	free_irq(gsi->irq, gsi);
	mutex_destroy(&gsi->mutex);
err_unmap_virt:
	iounmap(gsi->virt);

	return ret;
}

/* Inverse of gsi_init() */
void gsi_exit(struct gsi *gsi)
{
	gsi_channel_exit(gsi);

	if (gsi->irq_wake_enabled)
		(void)disable_irq_wake(gsi->irq);
	free_irq(gsi->irq, gsi);
	mutex_destroy(&gsi->mutex);
	iounmap(gsi->virt);
}

/* Returns the maximum number of pending transactions on a channel */
u32 gsi_channel_trans_max(struct gsi *gsi, u32 channel_id)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];

	return channel->data->tre_count;
}

/* Returns the maximum number of TREs in a single transaction for a channel */
u32 gsi_channel_trans_tre_max(struct gsi *gsi, u32 channel_id)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];

	return channel->data->tlv_count;
}

/* Wait for all transaction activity on a channel to complete */
void gsi_channel_trans_quiesce(struct gsi *gsi, u32 channel_id, bool stopping)
{
	struct gsi_trans *trans;

	/* Get the last transaction, and wait for it to complete */
	trans = gsi_channel_trans_last(gsi, channel_id, stopping);
	if (trans) {
		wait_for_completion(&trans->completion);
		gsi_trans_free(trans);
	}
}

/* Make a channel operational */
int gsi_channel_start(struct gsi *gsi, u32 channel_id)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];

	if (channel->state != GSI_CHANNEL_STATE_ALLOCATED &&
	    channel->state != GSI_CHANNEL_STATE_STOP_IN_PROC &&
	    channel->state != GSI_CHANNEL_STATE_STOPPED) {
		dev_err(gsi->dev, "channel %u bad state %u\n", channel_id,
			(u32)channel->state);
		return -ENOTSUPP;
	}

	napi_enable(&channel->napi);

	mutex_lock(&gsi->mutex);

	gsi_channel_command(channel, GSI_CH_START);
	gsi->channel_stats.start++;

	mutex_unlock(&gsi->mutex);

	return 0;
}

/* Stop an operational channel */
int gsi_channel_stop(struct gsi *gsi, u32 channel_id)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];
	int ret;

	if (channel->state == GSI_CHANNEL_STATE_STOPPED)
		return 0;

	if (channel->state != GSI_CHANNEL_STATE_STARTED &&
	    channel->state != GSI_CHANNEL_STATE_STOP_IN_PROC &&
	    channel->state != GSI_CHANNEL_STATE_ERROR) {
		dev_err(gsi->dev, "channel %u bad state %u\n", channel_id,
			(u32)channel->state);
		return -ENOTSUPP;
	}

	gsi_channel_trans_quiesce(gsi, channel_id, true);

	mutex_lock(&gsi->mutex);

	gsi_channel_command(channel, GSI_CH_STOP);
	gsi->channel_stats.stop++;

	mutex_unlock(&gsi->mutex);

	if (channel->state == GSI_CHANNEL_STATE_STOPPED)
		ret = 0;
	else if (channel->state == GSI_CHANNEL_STATE_STOP_IN_PROC)
		ret = -EAGAIN;
	else
		ret = -EIO;

	if (!ret)
		napi_disable(&channel->napi);

	return ret;
}

/* Reset and reconfigure a GSI channel (possibly leaving doorbell disabled) */
int gsi_channel_reset(struct gsi *gsi, u32 channel_id, bool db_enable)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];

	if (channel->state != GSI_CHANNEL_STATE_STOPPED) {
		dev_err(gsi->dev, "channel %u bad state %u\n", channel_id,
			(u32)channel->state);
		return -ENOTSUPP;
	}

	/* In case the reset follows stop, need to wait 1 msec */
	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	mutex_lock(&gsi->mutex);

	gsi_channel_command(channel, GSI_CH_RESET);
	gsi->channel_stats.reset++;

	/* workaround: reset RX channels again */
	if (!channel->toward_ipa) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		gsi_channel_command(channel, GSI_CH_RESET);
	}

	gsi_channel_config(channel, db_enable);

	/* Cancel pending transactions before the channel is started again */
	gsi_channel_trans_cancel_pending(channel);

	mutex_unlock(&gsi->mutex);

	return 0;
}
