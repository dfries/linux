/*
 * ssi_mcsaab_imp.c
 *
 * Implementation of the SSI McSAAB improved protocol.
 *
 * Copyright (C) 2007-2008 Nokia Corporation. All rights reserved.
 *
 * Contact: Carlos Chinea <carlos.chinea@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <linux/if_phonet.h>
#include <linux/ssi_driver_if.h>

#define __HANDSHAKE_FQ_CHANGE__	/* FIXME: HACK to be removed */

#define	MCSAAB_IMP_VERSION	"2.0-rc1"
#define MCSAAB_IMP_DESC		"SSI McSAAB Improved protocol implementeation"
#define MCSAAB_IMP_NAME		"SSI McSAAB PROTOCOL"

#define LOG_NAME	"McSAAB: "
/* ssi_proto flags values */

/* CMT online/offline */
#define CMT_ONLINE	0x01
/* Keep track of clocks */
#define CLK_ENABLE	0x02
/*
 * Flag use to check if the WAKELINE TEST down has been already made
 * This is needed to avoid the race condition where the CMT puts down
 * the CAWAKE line before we have processed the WAKELINE_TEST result.
 * If this is not done clock management in the HW driver will fail.
 */
#define WAKEDOWN_TEST	0x04
#define READY_SENT	0x08

/* end ssi_proto flags */

#define LOCAL_D_VER_ID	0x01

#define MCSAAB_TX_QUEUE_LEN	100

#define C_QUEUE_LEN	4

#define SSI_MAX_MTU	65535
#define SSI_DEFAULT_MTU	4000

#define CMT_DEFAULT_TX_SPEED	110	/* 110 Mbits/s */

#define WD_TIMEOUT	2000	/* 500 msecs */

#define PN_MEDIA_SOS	21
/*
 * McSAAB DEBUG
 */
#ifdef SSI_DEBUG
#define DBG(fmt, args...)	printk(KERN_DEBUG LOG_NAME "%s: " fmt "\n",\
							__func__, ##args)
#define DBG_DEV(dev, fmt, args...) dev_dbg(dev, LOG_NAME "%s: " fmt "\n",\
							__func__, ##args)
#define DBG_DUMP(buf, len)      print_hex_dump_bytes(LOG_NAME,\
					DUMP_PREFIX_ADDRESS, buf, len)
#else
#define DBG(fmt, args...)		do {} while (0)
#define DBG_DEV(dev, fmt, args...)	do {} while (0)
#define DBG_DUMP(buf, len)		do {} while (0)
#endif /* SSI_DEBUG */

/*
 * McSAAB command definitions
 */
#define COMMAND(data)	(data >> 28)
#define PAYLOAD(data)	(data & 0x0fffffff)

/* Commands */
#define SW_BREAK	0x0
#define BOOT_INFO_REQ	0x1
#define BOOT_INFO_RESP	0x2
#define WAKE_TEST_RES	0x3
#define START_TRANS	0x4
#define READY		0x5
#define FQ_CHANGE_REQ	0x8
#define FQ_CHANGE_DONE	0x9
#define ACK		0xa
#define DUMMY		0xc

/* Payloads */
#define RESERVED		0X0000000
#define DATA_VERSION_MASK	0xff
#define DATA_VERSION(data)	(data & DATA_VERSION_MASK)
#define DATA_RESULT_MASK	0X0f
#define DATA_RESULT(data)	(data & DATA_RESULT_MASK)
#define WAKE_TEST_OK		0x0
#define WAKE_TEST_FAILED	0x1
#define PDU_LENGTH_MASK		0xffff
#define PDU_LENGTH(data)	((data >> 8) & PDU_LENGTH_MASK)
#define MSG_ID_MASK		0xff
#define MSG_ID(data)		(data & MSG_ID_MASK)
#define ACK_TO_CMD_MASK		0x0f
#define ACK_TO_CMD(data)	(data & ACK_TO_CMD_MASK)

#define DUMMY_PAYLOAD		0xaaccaaa

#define CMD(command, payload) ((command<<28) | (payload & 0x0fffffff))

/* Commands for the control channel (channel number 0) */
#define SWBREAK_CMD			CMD(SW_BREAK, 0x000000)
#define BOOT_INFO_REQ_CMD(verid) \
				CMD(BOOT_INFO_REQ, verid & DATA_VERSION_MASK)
#define BOOT_INFO_RESP_CMD(verid) \
				CMD(BOOT_INFO_RESP, verid & DATA_VERSION_MASK)
#define START_TRANS_CMD(pdu_len, message_id) \
				CMD(START_TRANS, ((pdu_len << 8) | message_id))
#define READY_CMD			CMD(READY, RESERVED)
#define FQ_CHANGE_REQ_CMD(max_tx_speed)	CMD(FQ_CHANGE_REQ, max_tx_speed)
#define FQ_CHANGE_DONE_CMD		CMD(FQ_CHANGE_DONE, RESERVED)
#define ACK_CMD(ack_cmd)		CMD(ACK, cmd)

/* Special hardcoded message */
#define SKIP	0xf0030006
/*
 * End McSAAB command definitions
 */

/* Main state machine states */
enum {
	INIT,
	HANDSHAKE,
	ACTIVE,
	MAIN_NUM_STATES,	/* NOTE: Must be always the last one*/
};

/* Send state machine states */
enum {
	WAIT4READY,
	SEND_READY,
	SENDING,
	SENDING_SWBREAK,
	SEND_NUM_STATES,	/* NOTE: Must be always the last one */
};

/* Recevice state machine states */
enum {
	RECV_READY,
	RECEIVING,
	RECV_BUSY,
	RECV_NUM_STATES,	/* NOTE: Must be always the last one */
};

struct mcsaab_imp {
	unsigned int main_state;
	unsigned int send_state;
	unsigned int recv_state;
	unsigned int flags;

	u32 rcv_c_msg;

	u32 c_queue[C_QUEUE_LEN];
	int head;
	int tail;

	u8 rcv_msg_id;
	u8 send_msg_id;

	u16 cmt_default_tx_speed;
	u16 cmt_tx_speed;

	struct ssi_device *dev_d_ch;
	struct ssi_device *dev_c_ch;

	struct timer_list boot_wd;
	struct timer_list tx_wd;
	struct timer_list rx_wd;

	struct clk *ssi_clk;

	spinlock_t lock;

	/* Network interface */
	struct sk_buff_head tx_queue;
	struct sk_buff_head rx_queue;

	struct net_device *netdev;
};

static struct mcsaab_imp ssi_protocol;

static void mcsaab_clk_enable(void)
{
	if (!(ssi_protocol.flags & CLK_ENABLE)) {
		ssi_protocol.flags |= CLK_ENABLE;
		clk_enable(ssi_protocol.ssi_clk);
	}
}

static void mcsaab_clk_disable(void)
{
	if (ssi_protocol.flags & CLK_ENABLE) {
		ssi_protocol.flags &= ~CLK_ENABLE;
		clk_disable(ssi_protocol.ssi_clk);
	}
}

static void reset_mcsaab(void)
{
	mcsaab_clk_disable(); /* Release clk, if held */
	del_timer(&ssi_protocol.boot_wd);
	del_timer(&ssi_protocol.rx_wd);
	del_timer(&ssi_protocol.tx_wd);
	ssi_protocol.main_state = INIT;
	ssi_protocol.send_msg_id = 0;
	ssi_protocol.rcv_msg_id = 0;
	ssi_protocol.send_state = RECV_READY;
	ssi_protocol.recv_state = SEND_READY;
	ssi_protocol.flags = 0;
	ssi_protocol.head = 0;
	ssi_protocol.tail = 0;
	if (ssi_protocol.dev_d_ch) {
		ssi_read_cancel(ssi_protocol.dev_d_ch);
		ssi_write_cancel(ssi_protocol.dev_d_ch);
	}
	if (ssi_protocol.dev_c_ch)
		ssi_write_cancel(ssi_protocol.dev_c_ch);
	skb_queue_purge(&ssi_protocol.tx_queue);
	skb_queue_purge(&ssi_protocol.rx_queue);
	DBG("CMT is OFFLINE");
	netif_carrier_off(ssi_protocol.netdev);

}

static void send_c_msg(u32 c_msg)
{
	int size;

	size = (C_QUEUE_LEN + ssi_protocol.tail - ssi_protocol.head)
							% C_QUEUE_LEN;
	if (size >= (C_QUEUE_LEN - 1)) {
		printk(KERN_DEBUG LOG_NAME "Control message queue OVERRUN !\n");
		return;
	}
	DBG("Queue head %d tail %d size %d",
				ssi_protocol.head, ssi_protocol.tail, size);
	ssi_protocol.c_queue[ssi_protocol.tail] = c_msg;
	ssi_protocol.tail = (ssi_protocol.tail + 1) % C_QUEUE_LEN;

	if (size == 0)
		ssi_write(ssi_protocol.dev_c_ch,
				&ssi_protocol.c_queue[ssi_protocol.head], 1);

}

static void mcsaab_start_rx(int len)
{
	struct sk_buff *skb;

	skb = netdev_alloc_skb(ssi_protocol.netdev, len * 4);
	if (unlikely(!skb)) {
		printk(KERN_DEBUG LOG_NAME "Out of memory RX skb.\n");
		reset_mcsaab();
		return;
	}

	skb_put(skb, len * 4);
	skb_queue_tail(&ssi_protocol.rx_queue, skb);
	if (skb_queue_len(&ssi_protocol.rx_queue) == 1) {
		mod_timer(&ssi_protocol.rx_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		ssi_protocol.recv_state = RECEIVING;
		ssi_read(ssi_protocol.dev_d_ch, (u32 *)skb->data, len);
	}
}

static void mcsaab_start_tx(void)
{
	struct sk_buff *skb;

	skb = skb_peek(&ssi_protocol.tx_queue);
	ssi_protocol.send_state = SENDING;
	mod_timer(&ssi_protocol.tx_wd, jiffies + msecs_to_jiffies(WD_TIMEOUT));
	send_c_msg(START_TRANS_CMD((skb->len + 3) / 4,
						ssi_protocol.send_msg_id));
}

/* Watchdog functions */
static void mcsaab_watchdog_dump(struct mcsaab_imp *prot)
{
	struct sk_buff *skb;
	u32 acwake;
	unsigned int cawake;
	unsigned int last;

	ssi_ioctl(prot->dev_c_ch, SSI_IOCTL_WAKE, &acwake);
	ssi_ioctl(prot->dev_c_ch, SSI_IOCTL_CAWAKE, &cawake);

	last = (C_QUEUE_LEN - 1 + ssi_protocol.head) % C_QUEUE_LEN;
	printk(KERN_DEBUG LOG_NAME "ACWake line %08X\n", acwake);
	printk(KERN_DEBUG LOG_NAME "CAWake line %d\n", cawake);
	printk(KERN_DEBUG LOG_NAME "Main state: %d\n", prot->main_state);
	printk(KERN_DEBUG LOG_NAME "RX state:%02X\n", prot->recv_state);
	printk(KERN_DEBUG LOG_NAME "TX state:%02X\n", prot->send_state);
	printk(KERN_DEBUG LOG_NAME "CMT was %s\n",
			(prot->flags & CMT_ONLINE) ? "ONLINE" : "OFFLINE");
	printk(KERN_DEBUG LOG_NAME "FLAGS: %04X\n", prot->flags);
	printk(KERN_DEBUG LOG_NAME "Last RX control msg %08X\n",
							prot->rcv_c_msg);
	printk(KERN_DEBUG LOG_NAME "Last TX control msg %08X\n",
							prot->c_queue[last]);
	printk(KERN_DEBUG LOG_NAME "TX C queue head %d tail %d\n", prot->head,
								prot->tail);
	printk(KERN_DEBUG LOG_NAME "Data RX ID: %d\n", prot->rcv_msg_id);
	printk(KERN_DEBUG LOG_NAME "Data TX ID: %d\n", prot->send_msg_id);
	printk(KERN_DEBUG LOG_NAME "TX queue len: %d\n",
						skb_queue_len(&prot->tx_queue));
	if (skb_queue_len(&prot->tx_queue) > 0) {
		skb = skb_peek(&prot->tx_queue);
		printk(KERN_DEBUG LOG_NAME "TX HEAD packet:\n");
		print_hex_dump_bytes(LOG_NAME, DUMP_PREFIX_ADDRESS, skb->data,
					min(skb->len, (unsigned int)32));
		printk(KERN_DEBUG LOG_NAME "END TX HEAD packet.\n");
	}
	printk(KERN_DEBUG LOG_NAME "RX queue len: %d\n",
						skb_queue_len(&prot->rx_queue));
	if (skb_queue_len(&prot->rx_queue) > 0) {
		skb = skb_peek(&prot->rx_queue);
		printk(KERN_DEBUG LOG_NAME "RX HEAD packet:\n");
		print_hex_dump_bytes(LOG_NAME, DUMP_PREFIX_ADDRESS, skb->data,
					min(skb->len, (unsigned int)32));
		printk(KERN_DEBUG LOG_NAME "END RX HEAD packet.\n");

	}
}

static void mcsaab_watchdog(unsigned long data)
{
	struct mcsaab_imp *prot = (struct mcsaab_imp *)data;
	DBG("------ WATCHDOG TIMER trigerred ------\n");
	mcsaab_watchdog_dump(prot);
	DBG("--------------------------------------\n");

	reset_mcsaab();
	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_DOWN, NULL);
}

static void mcsaab_watchdog_rx(unsigned long data)
{
	DBG("------- RX WATCHDOG TIMER trigerred -----\n");

	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_FLUSH_RX, NULL);
	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_FLUSH_TX, NULL);
	mcsaab_watchdog(data);
}

static void mcsaab_watchdog_tx(unsigned long data)
{
	DBG("------- TX WATCHDOG TIMER trigerred -----\n");
	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_FLUSH_RX, NULL);
	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_FLUSH_TX, NULL);
	mcsaab_watchdog(data);
}

/* End watchdog functions */

/*
 * Network device callbacks
 */
static int ssi_pn_xmit(struct sk_buff *skb, struct net_device *dev)
{
	u32 acwake = 0;
	int qlen;

	if (skb->protocol != htons(ETH_P_PHONET))
		goto drop;

	/* Pad to 32-bits */
	if ((skb->len & 3) && skb_pad(skb, 4 - (skb->len & 3))) {
		dev->stats.tx_dropped++;
		return 0;
	}

	/* Modem sends Phonet messages over SSI with its own endianess...
	 * Assume that modem has the same endianess as we do. */
	if (skb_cow_head(skb, 0))
		goto drop;
#ifdef __LITTLE_ENDIAN
	if (likely(skb->len >= 6)) {
		u8 buf = skb->data[4];
		skb->data[4] = skb->data[5];
		skb->data[5] = buf;
	}
#endif

	spin_lock_bh(&ssi_protocol.lock);

	if (unlikely(!(ssi_protocol.flags & CMT_ONLINE))) {
		pr_notice(LOG_NAME "Dropping TX data. CMT is OFFLINE\n");
		spin_unlock_bh(&ssi_protocol.lock);
		goto drop;
	}

	skb_queue_tail(&ssi_protocol.tx_queue, skb);
	qlen = skb_queue_len(&ssi_protocol.tx_queue);

	if ((dev->tx_queue_len > 1) && (qlen >= dev->tx_queue_len)) {
		DBG("TX queue full %d", qlen);
		netif_stop_queue(dev);
		goto out;
	} else if (qlen > 1) {
		DBG("Pending frame on TX queue %d", qlen);
		goto out;
	}

	/*
	 * Check if ACWAKE line is down. We need to check if audio driver
	 * has put the wakeline down so we know that we need to wait for a
	 * READY command when McSAAB sets it up.
	 */
	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE, &acwake);
	DBG("ACWAKE %d", acwake);
	if (!acwake)
		ssi_protocol.send_state = WAIT4READY;

	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_UP, NULL);
	ssi_protocol.main_state = ACTIVE;
	if (ssi_protocol.send_state == SEND_READY)
		mcsaab_start_tx();
	else {
		DBG("TX pending of READY cmd");
		mod_timer(&ssi_protocol.tx_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
	}
out:
	spin_unlock_bh(&ssi_protocol.lock);
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
	return 0;

drop:
	dev->stats.tx_dropped++;
	dev_kfree_skb(skb);
	return 0;
}

static int ssi_pn_set_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu > SSI_MAX_MTU || new_mtu < PHONET_MIN_MTU)
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

static void ssi_pn_setup(struct net_device *dev)
{
	dev->features		= 0;
	dev->type		= ARPHRD_PHONET;
	dev->flags		= IFF_POINTOPOINT | IFF_NOARP;
	dev->mtu		= SSI_DEFAULT_MTU;
	dev->hard_header_len	= 1;
	dev->dev_addr[0]	= PN_MEDIA_SOS;
	dev->addr_len		= 1;
	dev->tx_queue_len	= MCSAAB_TX_QUEUE_LEN;

	dev->destructor		= free_netdev;
	dev->header_ops		= &phonet_header_ops;
	dev->hard_start_xmit	= ssi_pn_xmit; /* mandatory */
	dev->change_mtu		= ssi_pn_set_mtu;
}

/* In soft IRQ context */
static int ssi_pn_rx(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;

	if (unlikely(!netif_running(dev))) {
		dev->stats.rx_dropped++;
		goto drop;
	}
	if (unlikely(!pskb_may_pull(skb, 6))) {
		dev->stats.rx_errors++;
		dev->stats.rx_length_errors++;
		goto drop;
	}

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += skb->len;

#ifdef __LITTLE_ENDIAN
	if (likely(skb->len >= 6))
		((u16 *)skb->data)[2] = swab16(((u16 *)skb->data)[2]);
	DBG("RX length fixed (%04x -> %u)", ((u16 *)skb->data)[2],
		ntohs(((u16 *)skb->data)[2]));
#endif

	skb->protocol = htons(ETH_P_PHONET);
	skb_reset_mac_header(skb);
	__skb_pull(skb, 1);

	DBG("RX done");
	netif_rx(skb);
	return 0;

drop:
	DBG("Drop RX packet");
	dev_kfree_skb(skb);
	return 0;
}
/*
 * End network device callbacks
 */

/* Incoming commands */
static void boot_info_req_h(u32 msg)
{
	switch (ssi_protocol.main_state) {
	case INIT:
		mcsaab_clk_enable();
		send_c_msg(BOOT_INFO_RESP_CMD(LOCAL_D_VER_ID));
		ssi_protocol.main_state = HANDSHAKE;
		/* Start BOOT HANDSHAKE timer */
		mod_timer(&ssi_protocol.boot_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		break;
	case HANDSHAKE:
		send_c_msg(BOOT_INFO_RESP_CMD(LOCAL_D_VER_ID));
		/* Start BOOT HANDSHAKE timer */
		mod_timer(&ssi_protocol.boot_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		break;
	case ACTIVE:
		pr_warning(LOG_NAME "Rebooting sequence started.\n");
		mcsaab_watchdog_dump(&ssi_protocol);
		reset_mcsaab();
		mcsaab_clk_enable();
		ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_UP, NULL);
		send_c_msg(BOOT_INFO_RESP_CMD(LOCAL_D_VER_ID));
		ssi_protocol.main_state = HANDSHAKE;
		/* Start BOOT HANDSHAKE timer */
		mod_timer(&ssi_protocol.boot_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		break;
	default:
		DBG("UNKNOWN PROTOCOL STATE %d", ssi_protocol.main_state);
		break;
	}
}

static void boot_info_resp_h(u32 msg)
{
	if (ssi_protocol.main_state != INIT) {
		DBG("BOOT_INFO_RESP in bad state:");
		DBG("	MAIN_STATE %d", ssi_protocol.main_state);
		return;
	}

	mcsaab_clk_enable();
	ssi_protocol.main_state = HANDSHAKE;
}

static void wakelines_test_result_h(u32 msg)
{
	if (ssi_protocol.main_state != HANDSHAKE) {
		DBG("WAKELINES_TEST in bad state:");
		DBG("	MAIN_STATE %d", ssi_protocol.main_state);
		return;
	}

	pr_notice(LOG_NAME "WAKELINES TEST %s\n",
			(PAYLOAD(msg) & WAKE_TEST_FAILED) ? "FAILED" : "OK");

	if (PAYLOAD(msg) & WAKE_TEST_FAILED) {
		mcsaab_watchdog_dump(&ssi_protocol);
		reset_mcsaab();
	} else {
#ifdef __HANDSHAKE_FQ_CHANGE__
		send_c_msg(FQ_CHANGE_REQ_CMD(ssi_protocol.cmt_tx_speed));
		return;
#else
		ssi_protocol.main_state = ACTIVE;
		ssi_protocol.flags &= ~WAKEDOWN_TEST;
		ssi_protocol.flags |= CMT_ONLINE;
		DBG("CMT is ONLINE");
		netif_carrier_on(ssi_protocol.netdev);
		netif_wake_queue(ssi_protocol.netdev);
#endif
	}
	ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_DOWN, NULL);
	mcsaab_clk_disable(); /* Drop clk usecount */
	/* Stop BOOT HANDSHAKE timer */
	del_timer(&ssi_protocol.boot_wd);
}

static void ack_to_cmd_h(u32 msg)
{
	u8 cmd_id;

	cmd_id = ACK_TO_CMD(msg);
	pr_debug(LOG_NAME "ACK to command %d\n", cmd_id);

	switch (cmd_id) {
	case FQ_CHANGE_REQ:
#ifdef __HANDSHAKE_FQ_CHANGE__
		if (ssi_protocol.main_state == HANDSHAKE)
			send_c_msg(FQ_CHANGE_DONE_CMD);
#endif
		break;
	default:
		break;
	}
}

static void start_trans_h(u32 msg)
{
	u8 r_msg_id = 0;

	r_msg_id = msg & MSG_ID_MASK;
	DBG("Receiving START_TRANS len %d", PDU_LENGTH(msg));
	DBG("START_TRANS msg id %d expected msg id %d",
					r_msg_id, ssi_protocol.rcv_msg_id);

	if (unlikely(ssi_protocol.main_state != ACTIVE)) {
		DBG("START_TRANS in bad state:\n");
		DBG("	SEND STATE %d", ssi_protocol.send_state);
		DBG("	MAIN_STATE %d", ssi_protocol.main_state);
		return;
	}

	if (unlikely(r_msg_id != ssi_protocol.rcv_msg_id)) {
		printk(KERN_DEBUG LOG_NAME "RX msg id mismatch (MSG ID: %d "
		"McSAAB RX ID: %d)\n", r_msg_id, ssi_protocol.rcv_msg_id);
		mcsaab_watchdog_dump(&ssi_protocol);
		reset_mcsaab();
		return;
	}
	ssi_protocol.rcv_msg_id = (ssi_protocol.rcv_msg_id + 1) & 0xff;
	ssi_protocol.flags &= ~READY_SENT;
	mcsaab_start_rx(PDU_LENGTH(msg));
}

static void ready_h(u32 msg)
{
	if (unlikely((ssi_protocol.main_state != ACTIVE) ||
					(ssi_protocol.send_state >= SENDING))) {
		DBG("READY CMD on bad state:");
		DBG("	SEND STATE %d", ssi_protocol.send_state);
		DBG("	MAIN_STATE %d", ssi_protocol.main_state);
		DBG("	FLAGS %02X", ssi_protocol.flags);
		return;
	}
	if (skb_queue_len(&ssi_protocol.tx_queue) > 0)
		mcsaab_start_tx();
	else
		ssi_protocol.send_state = SEND_READY;
}

static void swbreak_h(void)
{
	if (ssi_protocol.main_state != ACTIVE) {
		DBG("SW BREAK in bad state:\n");
		DBG("	SEND STATE %d", ssi_protocol.send_state);
		DBG("	MAIN_STATE %d", ssi_protocol.main_state);
		return;
	}
	DBG("SWBREAK Ignored");
	mcsaab_clk_disable();
}
/* End incoming commands */

/* OMAP SSI driver callbacks */
static void c_send_done_cb(struct ssi_device *c_dev)
{
	u32 acwake = 0;
	u32 cmd;
	struct sk_buff *skb;


	spin_lock(&ssi_protocol.lock);

	cmd = ssi_protocol.c_queue[ssi_protocol.head];
	DBG("Control message 0x%08X sent", cmd);

	if ((COMMAND(cmd) == START_TRANS) &&
			(ssi_protocol.send_state == SENDING)) {
		skb = skb_peek(&ssi_protocol.tx_queue);
		ssi_write(ssi_protocol.dev_d_ch, (u32 *)skb->data,
							(skb->len + 3) / 4);
	} else if ((COMMAND(cmd) == SW_BREAK) &&
			(ssi_protocol.send_state == SENDING_SWBREAK)) {
		if (skb_queue_len(&ssi_protocol.tx_queue) > 0) {
			DBG("We got SKB while sending SW_BREAK");
			mcsaab_start_tx();
		} else {
			DBG("SW BREAK: Trying to set ACWake line DOWN");
			ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_DOWN,
									NULL);
			/*
			 * We need to check that other modules does not hold
			 * still the wakeup line.
			 */
			ssi_ioctl(c_dev, SSI_IOCTL_WAKE, &acwake);
			DBG("ACWAKE %d", acwake);
			if (!acwake)
				ssi_protocol.send_state = WAIT4READY;
			else
				ssi_protocol.send_state = SEND_READY;
		}
		netif_wake_queue(ssi_protocol.netdev);
#ifdef __HANDSHAKE_FQ_CHANGE__
	} else if ((COMMAND(cmd) == FQ_CHANGE_DONE) &&
				(ssi_protocol.main_state == HANDSHAKE)) {
		ssi_protocol.main_state = ACTIVE;
		ssi_protocol.flags &= ~WAKEDOWN_TEST;
		ssi_protocol.flags |= CMT_ONLINE;
		DBG("CMT is ONLINE");
		netif_carrier_on(ssi_protocol.netdev);
		netif_wake_queue(ssi_protocol.netdev);
		ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_DOWN, NULL);
		mcsaab_clk_disable(); /* Drop clk usecount */
		/* Stop BOOT HANDSHAKE timer */
		del_timer(&ssi_protocol.boot_wd);
	}
#else
	}
#endif
	/* Check for pending TX commands */
	++ssi_protocol.head;
	ssi_protocol.head %= C_QUEUE_LEN;

	if (ssi_protocol.tail != ssi_protocol.head) {
		DBG("Dequeue message on pos %d", ssi_protocol.head);
		DBG("Sending queued msg 0x%08x",
				ssi_protocol.c_queue[ssi_protocol.head]);
		ssi_write(ssi_protocol.dev_c_ch,
				&ssi_protocol.c_queue[ssi_protocol.head], 1);
	}

	spin_unlock(&ssi_protocol.lock);
}

static void d_send_done_cb(struct ssi_device *d_dev)
{
	struct sk_buff *skb;

	spin_lock(&ssi_protocol.lock);
	skb = skb_dequeue(&ssi_protocol.tx_queue);
	if (!skb)
		goto out;
	del_timer(&ssi_protocol.tx_wd);
	dev_kfree_skb(skb);
	ssi_protocol.send_msg_id++;
	ssi_protocol.send_msg_id &= 0xff;
	if (skb_queue_len(&ssi_protocol.tx_queue) <= 0) {
		DBG("Sending SWBREAK");
		send_c_msg(SWBREAK_CMD);
		ssi_protocol.send_state = SENDING_SWBREAK;
	} else {
		mcsaab_start_tx();
	}
out:
	spin_unlock(&ssi_protocol.lock);
}

static void c_rcv_done_cb(struct ssi_device *c_dev)
{
	u32 message = ssi_protocol.rcv_c_msg;
	unsigned int command = COMMAND(message);

	spin_lock(&ssi_protocol.lock);

	ssi_read(c_dev, &ssi_protocol.rcv_c_msg, 1);

	DBG("Protocol state %d", ssi_protocol.main_state);
	DBG("CMT Message 0x%08X CMD %01X", message, COMMAND(message));

	switch (command) {
	case SW_BREAK:
		swbreak_h();
		break;
	case BOOT_INFO_REQ:
		boot_info_req_h(message);
		break;
	case BOOT_INFO_RESP:
		boot_info_resp_h(message);
		break;
	case WAKE_TEST_RES:
		wakelines_test_result_h(message);
		break;
	case START_TRANS:
		start_trans_h(message);
		break;
	case READY:
		ready_h(message);
		break;
	case ACK:
		ack_to_cmd_h(message);
		break;
	case DUMMY:
		pr_warning(LOG_NAME "Received dummy sync 0x%08x\n", message);
		pr_warning(LOG_NAME "OLD McSAAB Protocol DETECTED\n");
		pr_warning(LOG_NAME "OLD PROTOCOL NOT SUPPORTED\n");
		break;
	default:
		pr_warning(LOG_NAME "COMMAND NOT SUPPORTED\n");
		pr_warning(LOG_NAME "Message 0x%08X\n", message);
		break;
	}
	spin_unlock(&ssi_protocol.lock);
}

static void d_rcv_done_cb(struct ssi_device *d_dev)
{
	struct sk_buff *skb;

	spin_lock(&ssi_protocol.lock);

	skb = skb_dequeue(&ssi_protocol.rx_queue);
	if (!skb)
		goto out;
	skb->dev = ssi_protocol.netdev;
	del_timer(&ssi_protocol.rx_wd); /* Stop RX timer */
	ssi_protocol.recv_state = RECV_READY;
	ssi_pn_rx(skb);
	if (skb_queue_len(&ssi_protocol.rx_queue) > 0) {
		skb = skb_peek(&ssi_protocol.rx_queue);
		mod_timer(&ssi_protocol.rx_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		ssi_protocol.recv_state = RECEIVING;
		pr_debug(LOG_NAME "Data len: %d\n", skb->len / 4);
		ssi_read(ssi_protocol.dev_d_ch, (u32 *)skb->data, skb->len / 4);
	}
out:
	spin_unlock(&ssi_protocol.lock);
}

static void wake_up_event(struct ssi_device *c_dev)
{
	switch (ssi_protocol.main_state) {
	case INIT:
		ssi_ioctl(c_dev, SSI_IOCTL_WAKE_UP, NULL);
		break;
	case HANDSHAKE:
		if (ssi_protocol.flags & WAKEDOWN_TEST) {
			/* Need this safeguard to avoid race condition */
			pr_notice(LOG_NAME "ACWAKE UP\n");
			ssi_ioctl(c_dev, SSI_IOCTL_WAKE_UP, NULL);
		}
		break;
	case ACTIVE:
		if (ssi_protocol.flags & READY_SENT) {
			/*
			 * We can have two UP events in a row due to a short low
			 * high transition. Therefore we need to ignore the
			 * sencond UP event.
			 */
			DBG("IGNORE 2nd CAWAKE UP");
			ssi_protocol.flags &= ~READY_SENT;
		} else {
			ssi_protocol.flags |= READY_SENT;
			mcsaab_clk_enable();
			send_c_msg(READY_CMD);
			/* Start RX timer */
			mod_timer(&ssi_protocol.rx_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		}
		break;
	default:
		DBG(LOG_NAME "UNKNOWN PROTOCOL STATE %d\n",
						ssi_protocol.main_state);
		break;
	}
}

static void wake_down_event(struct ssi_device *c_dev)
{
	DBG("WAKE DOWN in state %d", ssi_protocol.main_state);

	switch (ssi_protocol.main_state) {
	case INIT:
		break;
	case HANDSHAKE:
		if (!(ssi_protocol.flags & WAKEDOWN_TEST)) {
			/* Need this safeguard to avoid race condition */
			pr_notice(LOG_NAME "ACWAKE DOWN\n");
			ssi_ioctl(c_dev, SSI_IOCTL_WAKE_DOWN, NULL);
			ssi_protocol.flags |= WAKEDOWN_TEST;
		}
		break;
	case ACTIVE:
		break;
	default:
		DBG("UNKNOWN PROTOCOL STATE %d\n",
						ssi_protocol.main_state);
		break;
	}
}

static void port_event_cb(struct ssi_device *ssi_dev, unsigned int event,
								void *arg)
{
	DBG("Event %d: ", event);
	DBG("	on controller %d on port %d", c_id, port);
	DBG("	on protocol state %d", ssi_protocol.main_state);

	switch (event) {
	case SSI_EVENT_BREAK_DETECTED:
		pr_notice(LOG_NAME "BREAK DETECTED.\n");
		pr_warning(LOG_NAME "Rebooting sequence started...\n");
		mcsaab_watchdog_dump(&ssi_protocol);
		reset_mcsaab();
		ssi_ioctl(ssi_protocol.dev_c_ch, SSI_IOCTL_WAKE_UP, NULL);
		send_c_msg(SKIP);
		break;
	/* FIXME: Handle ERRORs */
	case SSI_EVENT_CAWAKE_UP:
		wake_up_event(ssi_protocol.dev_c_ch);
		break;
	case SSI_EVENT_CAWAKE_DOWN:
		wake_down_event(ssi_protocol.dev_c_ch);
		break;
	default:
		DBG("Recevived an UNKNOWN  event");
		break;
	}
}
/* End OMAP SSI callabcks */

static int __devinit open_ssi_hw_drv(struct mcsaab_imp *prot)
{
	int err = 0;
	unsigned int cawake = 0;

	err = ssi_open(prot->dev_c_ch);
	if (err < 0) {
		pr_err(LOG_NAME "Could not open CONTROL channel 0\n");
		goto rback1;
	}
	err = ssi_open(prot->dev_d_ch);
	if (err < 0) {
		pr_err(LOG_NAME "Could not open DATA channel 3\n");
		goto rback2;
	}

	DBG("Submitting read on the control channel");
	err = ssi_read(prot->dev_c_ch, &prot->rcv_c_msg, 1);
	if (err < 0) {
		pr_err(LOG_NAME "Error when submiting first control read\n");
		goto rback3;
	}
	ssi_ioctl(prot->dev_c_ch, SSI_IOCTL_CAWAKE, &cawake);
	if (cawake) {
		/* Start BOOT HANDSHAKE timer */
		mod_timer(&ssi_protocol.boot_wd,
					jiffies + msecs_to_jiffies(WD_TIMEOUT));
		ssi_ioctl(prot->dev_c_ch, SSI_IOCTL_WAKE_UP, NULL);
		send_c_msg(BOOT_INFO_REQ_CMD(0x1));
	}

	return 0;
rback3:
	ssi_close(prot->dev_d_ch);
rback2:
	ssi_close(prot->dev_c_ch);
rback1:
	return err;
}

static int __devinit mcsaab_probe(struct ssi_device *ssi_dev)
{
	int err = 0;

	if ((ssi_dev->n_ch == 0) && (ssi_dev->n_p == 0)) {
		ssi_set_read_cb(ssi_dev, c_rcv_done_cb);
		ssi_set_write_cb(ssi_dev, c_send_done_cb);
		ssi_set_port_event_cb(ssi_dev, port_event_cb);
		spin_lock_bh(&ssi_protocol.lock);
		ssi_protocol.dev_c_ch = ssi_dev;
		spin_unlock_bh(&ssi_protocol.lock);
	} else if ((ssi_dev->n_ch == 3) && (ssi_dev->n_p == 0)) {
		ssi_set_read_cb(ssi_dev, d_rcv_done_cb);
		ssi_set_write_cb(ssi_dev, d_send_done_cb);
		spin_lock_bh(&ssi_protocol.lock);
		ssi_protocol.dev_d_ch = ssi_dev;
		spin_unlock_bh(&ssi_protocol.lock);
	} else
		return -ENXIO;

	spin_lock_bh(&ssi_protocol.lock);

	if ((ssi_protocol.dev_d_ch) && (ssi_protocol.dev_c_ch))
		err = open_ssi_hw_drv(&ssi_protocol);

	spin_unlock_bh(&ssi_protocol.lock);

	return err;
}

static int __devexit mcsaab_remove(struct ssi_device *ssi_dev)
{
	spin_lock_bh(&ssi_protocol.lock);
	if (ssi_protocol.flags & CMT_ONLINE)
		netif_carrier_off(ssi_protocol.netdev);

	if (ssi_dev == ssi_protocol.dev_c_ch) {
		ssi_protocol.main_state = INIT;
		ssi_protocol.send_state = SEND_READY;
		ssi_protocol.recv_state = RECV_READY;
		ssi_protocol.flags = 0;
		ssi_protocol.head = 0;
		ssi_protocol.tail = 0;
		ssi_protocol.dev_c_ch = NULL;
	} else if (ssi_dev == ssi_protocol.dev_d_ch) {
		ssi_protocol.dev_d_ch = NULL;
	}
	spin_unlock_bh(&ssi_protocol.lock);
	ssi_set_read_cb(ssi_dev, NULL);
	ssi_set_write_cb(ssi_dev, NULL);
	ssi_set_port_event_cb(ssi_dev, NULL);
	ssi_close(ssi_dev);

	return 0;
}

static struct ssi_device_driver ssi_mcsaab_driver = {
	.ctrl_mask = ANY_SSI_CONTROLLER,
	.ch_mask[0] = CHANNEL(0) | CHANNEL(3),
	.probe = mcsaab_probe,
	.remove = __devexit_p(mcsaab_remove),
	.driver = {
			.name = "ssi_mcsaab_imp",
	},
};

/* NOTE: Notice that the WAKE line test. Must be done between 1ms time.
 * So too much DEBUG information can provoke that we are late and
 * failed the handshaking !*/
static int __init ssi_proto_init(void)
{
	static const char ifname[] = "phonet%d";
	int err = 0;

	pr_info(MCSAAB_IMP_NAME " Version: " MCSAAB_IMP_VERSION "\n");

	spin_lock_init(&ssi_protocol.lock);
	init_timer(&ssi_protocol.boot_wd);
	init_timer(&ssi_protocol.rx_wd);
	init_timer(&ssi_protocol.tx_wd);
	ssi_protocol.cmt_default_tx_speed = CMT_DEFAULT_TX_SPEED;
	ssi_protocol.cmt_tx_speed = 55; /* FIXME */
	ssi_protocol.main_state = INIT;
	ssi_protocol.send_state = SEND_READY;
	ssi_protocol.recv_state = RECV_READY;
	ssi_protocol.flags = 0;
	ssi_protocol.head = 0;
	ssi_protocol.tail = 0;
	ssi_protocol.dev_c_ch = NULL;
	ssi_protocol.dev_d_ch = NULL;
	ssi_protocol.boot_wd.data = (unsigned long)&ssi_protocol;
	ssi_protocol.boot_wd.function = mcsaab_watchdog;
	ssi_protocol.rx_wd.data = (unsigned long)&ssi_protocol;
	ssi_protocol.rx_wd.function = mcsaab_watchdog_rx;
	ssi_protocol.tx_wd.data = (unsigned long)&ssi_protocol;
	ssi_protocol.tx_wd.function = mcsaab_watchdog_tx;
	ssi_protocol.ssi_clk = NULL;
	skb_queue_head_init(&ssi_protocol.tx_queue);
	skb_queue_head_init(&ssi_protocol.rx_queue);

	ssi_protocol.netdev = alloc_netdev(0, ifname, ssi_pn_setup);
	if (!ssi_protocol.netdev)
		return -ENOMEM;

	/* FIXME: */
	/*SET_NETDEV_DEV(ssi_protoco.netdev, &p->dev_d_ch->device);*/
	netif_carrier_off(ssi_protocol.netdev);
	err = register_netdev(ssi_protocol.netdev);
	if (err) {
		free_netdev(ssi_protocol.netdev);
		return err;
	}

	ssi_protocol.ssi_clk = clk_get(NULL, "ssi_clk");
	if (IS_ERR(ssi_protocol.ssi_clk)) {
		printk(KERN_DEBUG LOG_NAME "Could not claim SSI fck clock\n");
		err = PTR_ERR(ssi_protocol.ssi_clk);
		goto rback1;
	}

	err = register_ssi_driver(&ssi_mcsaab_driver);
	if (err < 0) {
		pr_err(LOG_NAME "Error when registering ssi driver: %d\n", err);
		goto rback2;
	}

	return 0;
rback2:
	clk_put(ssi_protocol.ssi_clk);
rback1:
	unregister_netdev(ssi_protocol.netdev);
	return err;
}

static void __exit ssi_proto_exit(void)
{
	reset_mcsaab();
	unregister_ssi_driver(&ssi_mcsaab_driver);
	clk_put(ssi_protocol.ssi_clk);
	unregister_netdev(ssi_protocol.netdev);

	pr_info(MCSAAB_IMP_NAME "REMOVED\n");
}

module_init(ssi_proto_init);
module_exit(ssi_proto_exit);

MODULE_ALIAS("ssi:omap_ssi-p0.c0");
MODULE_AUTHOR("Carlos Chinea, Remi Denis-Courmont, Nokia");
MODULE_DESCRIPTION(MCSAAB_IMP_DESC);
MODULE_LICENSE("GPL");
