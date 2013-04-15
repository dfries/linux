/*
 * MUSB OTG driver core code
 *
 * Copyright 2005 Mentor Graphics Corporation
 * Copyright (C) 2005-2006 by Texas Instruments
 * Copyright (C) 2006-2007 Nokia Corporation
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
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Inventra (Multipoint) Dual-Role Controller Driver for Linux.
 *
 * This consists of a Host Controller Driver (HCD) and a peripheral
 * controller driver implementing the "Gadget" API; OTG support is
 * in the works.  These are normal Linux-USB controller drivers which
 * use IRQs and have no dedicated thread.
 *
 * This version of the driver has only been used with products from
 * Texas Instruments.  Those products integrate the Inventra logic
 * with other DMA, IRQ, and bus modules, as well as other logic that
 * needs to be reflected in this driver.
 *
 *
 * NOTE:  the original Mentor code here was pretty much a collection
 * of mechanisms that don't seem to have been fully integrated/working
 * for *any* Linux kernel version.  This version aims at Linux 2.6.now,
 * Key open issues include:
 *
 *  - Lack of host-side transaction scheduling, for all transfer types.
 *    The hardware doesn't do it; instead, software must.
 *
 *    This is not an issue for OTG devices that don't support external
 *    hubs, but for more "normal" USB hosts it's a user issue that the
 *    "multipoint" support doesn't scale in the expected ways.  That
 *    includes DaVinci EVM in a common non-OTG mode.
 *
 *      * Control and bulk use dedicated endpoints, and there's as
 *        yet no mechanism to either (a) reclaim the hardware when
 *        peripherals are NAKing, which gets complicated with bulk
 *        endpoints, or (b) use more than a single bulk endpoint in
 *        each direction.
 *
 *        RESULT:  one device may be perceived as blocking another one.
 *
 *      * Interrupt and isochronous will dynamically allocate endpoint
 *        hardware, but (a) there's no record keeping for bandwidth;
 *        (b) in the common case that few endpoints are available, there
 *        is no mechanism to reuse endpoints to talk to multiple devices.
 *
 *        RESULT:  At one extreme, bandwidth can be overcommitted in
 *        some hardware configurations, no faults will be reported.
 *        At the other extreme, the bandwidth capabilities which do
 *        exist tend to be severely undercommitted.  You can't yet hook
 *        up both a keyboard and a mouse to an external USB hub.
 */

/*
 * This gets many kinds of configuration information:
 *	- Kconfig for everything user-configurable
 *	- platform_device for addressing, irq, and platform_data
 *	- platform_data is mostly for board-specific informarion
 *	  (plus recentrly, SOC or family details)
 *
 * Most of the conditional compilation will (someday) vanish.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#ifdef	CONFIG_ARM
#include <mach/hardware.h>
#include <mach/memory.h>
#include <asm/mach-types.h>
#endif

#include <mach/board-rx51.h>

#include "musb_core.h"


#ifdef CONFIG_ARCH_DAVINCI
#include "davinci.h"
#endif

static struct musb *the_musb;
static struct musb_ctx ctx;

#ifndef CONFIG_MUSB_PIO_ONLY
static int __initdata use_dma = 1;
#else
static int __initdata use_dma;
#endif
module_param(use_dma, bool, 0);
MODULE_PARM_DESC(use_dma, "enable/disable use of DMA");

unsigned musb_debug = 0;
module_param_named(debug, musb_debug, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug message level. Default = 0");

#define DRIVER_AUTHOR "Mentor Graphics, Texas Instruments, Nokia"
#define DRIVER_DESC "Inventra Dual-Role USB Controller Driver"

#define MUSB_VERSION "6.0"

#define DRIVER_INFO DRIVER_DESC ", v" MUSB_VERSION

#define MUSB_DRIVER_NAME "musb_hdrc"
const char musb_driver_name[] = MUSB_DRIVER_NAME;

MODULE_DESCRIPTION(DRIVER_INFO);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" MUSB_DRIVER_NAME);

static inline int musb_verify_charger(void __iomem *addr)
{
	u8 r, ret = 0;

	/* Reset the transceiver */
	r = musb_ulpi_readb(addr, ISP1704_FUNC_CTRL);
	r |= ISP1704_FUNC_CTRL_RESET;
	musb_ulpi_writeb(addr, ISP1704_FUNC_CTRL, r);
	msleep(1);

	/* Set normal mode */
	r &= ~(ISP1704_FUNC_CTRL_RESET | (3 << ISP1704_FUNC_CTRL_OPMODE));
	musb_ulpi_writeb(addr, ISP1704_FUNC_CTRL, r);

	/* Clear the DP and DM pull-down bits */
	r = musb_ulpi_readb(addr, ISP1704_OTG_CTRL);
	r &= ~(ISP1704_OTG_CTRL_DP_PULLDOWN | ISP1704_OTG_CTRL_DM_PULLDOWN);
	musb_ulpi_writeb(addr, ISP1704_OTG_CTRL, r);

	/* Enable strong pull-up on DP (1.5K) and reset */
	r = musb_ulpi_readb(addr, ISP1704_FUNC_CTRL);
	r |= ISP1704_FUNC_CTRL_TERMSELECT | ISP1704_FUNC_CTRL_RESET;
	musb_ulpi_writeb(addr, ISP1704_FUNC_CTRL, r);
	msleep(1);

	/* Read the line state */
	if (musb_ulpi_readb(addr, ISP1704_DEBUG)) {
		/* Is it a charger or PS2 connection */

		/* Enable weak pull-up resistor on DP */
		r = musb_ulpi_readb(addr, ISP1704_PWR_CTRL);
		r |= ISP1704_PWR_CTRL_DP_WKPU_EN;
		musb_ulpi_writeb(addr, ISP1704_PWR_CTRL, r);

		/* Disable strong pull-up on DP (1.5K) */
		r = musb_ulpi_readb(addr, ISP1704_FUNC_CTRL);
		r &= ~ISP1704_FUNC_CTRL_TERMSELECT;
		musb_ulpi_writeb(addr, ISP1704_FUNC_CTRL, r);

		/* Enable weak pull-down resistor on DM */
		r = musb_ulpi_readb(addr, ISP1704_OTG_CTRL);
		r |= ISP1704_OTG_CTRL_DM_PULLDOWN;
		musb_ulpi_writeb(addr, ISP1704_OTG_CTRL, r);

		/* It's a charger if the line states are clear */
		if (!(musb_ulpi_readb(addr, ISP1704_DEBUG)))
			ret = 1;

		/* Disable weak pull-up resistor on DP */
		r = musb_ulpi_readb(addr, ISP1704_PWR_CTRL);
		r &= ~ISP1704_PWR_CTRL_DP_WKPU_EN;
		musb_ulpi_writeb(addr, ISP1704_PWR_CTRL, r);
	} else {
		ret = 1;

		/* Disable strong pull-up on DP (1.5K) */
		r = musb_ulpi_readb(addr, ISP1704_FUNC_CTRL);
		r &= ~ISP1704_FUNC_CTRL_TERMSELECT;
		musb_ulpi_writeb(addr, ISP1704_FUNC_CTRL, r);
	}

	return ret;
}

/* Bad connections with the charger may lead into the transceiver
 * thinking that a device was just connected. We can wait for 5 ms to
 * ensure that these cases will generate SUSPEND interrupt and not
 * RESET. Reading and writing to the transceiver may still cause
 * RESET interrupts. We mask out RESET/RESUME interrupts to
 * recover from this.
 */
static int check_charger;
static int musb_charger_detect(struct musb *musb)
{
	unsigned long	timeout;

	u8              vdat = 0;
	u8              r;

	if (machine_is_nokia_rx51() && !rx51_with_charger_detection())
		return 0;

	msleep(5);

	/* Using ulpi with musb is quite tricky. The following code
	 * was written based on the ulpi application note.
	 *
	 * The order of reads and writes and quite important, don't
	 * change it unless you really know what you're doing
	 */

	DBG(4, "Some asshole called musb_charger_detect!");

	switch(musb->xceiv->state) {
		case OTG_STATE_B_IDLE:
			/* we always reset transceiver */
			check_charger = 1;

			/* HACK: ULPI tends to get stuck when booting with
			 * the cable connected
			 */
			r = musb_readb(musb->mregs, MUSB_DEVCTL);
			if ((r & MUSB_DEVCTL_VBUS)
					== (3 << MUSB_DEVCTL_VBUS_SHIFT)) {
				musb_save_ctx_and_suspend(&musb->g, 0);
				musb_restore_ctx_and_resume(&musb->g);
				if (musb->board && musb->board->set_pm_limits)
					musb->board->set_pm_limits(
							musb->controller, 1);
			}

			/* disable RESET and RESUME interrupts */
			r = musb_readb(musb->mregs, MUSB_INTRUSBE);
			r &= ~(MUSB_INTR_RESUME | MUSB_INTR_RESET);
			musb_writeb(musb->mregs, MUSB_INTRUSBE, r);

			if (musb->board && musb->board->xceiv_reset)
				musb->board->xceiv_reset();

			/* then we resume to sync with controller */
			r = musb_readb(musb->mregs, MUSB_POWER);
			musb_writeb(musb->mregs, MUSB_POWER,
					r | MUSB_POWER_RESUME);
			msleep(10);
			musb_writeb(musb->mregs, MUSB_POWER,
					r & ~MUSB_POWER_RESUME);

			/* now we set SW control bit in PWR_CTRL register */
			musb_ulpi_writeb(musb->mregs, ISP1704_PWR_CTRL,
					ISP1704_PWR_CTRL_SWCTRL);

			r = musb_ulpi_readb(musb->mregs, ISP1704_PWR_CTRL);
			r |= (ISP1704_PWR_CTRL_SWCTRL | ISP1704_PWR_CTRL_DPVSRC_EN);

			/* and finally enable manual charger detection */
			musb_ulpi_writeb(musb->mregs, ISP1704_PWR_CTRL, r);
			msleep(10);

			timeout = jiffies + msecs_to_jiffies(300);
			while (!time_after(jiffies, timeout)) {
				/* Check if there is a charger */
				vdat = !!(musb_ulpi_readb(musb->mregs, ISP1704_PWR_CTRL)
							& ISP1704_PWR_CTRL_VDAT_DET);
				if (vdat)
					break;
				msleep(1);
			}
			if (vdat)
				vdat = musb_verify_charger(musb->mregs);

			r &= ~ISP1704_PWR_CTRL_DPVSRC_EN;

			/* Clear DPVSRC_EN, otherwise usb communication doesn't work */
			musb_ulpi_writeb(musb->mregs, ISP1704_PWR_CTRL, r);
			break;

		default:
			vdat = 0;
			break;
	}

	if (vdat) {
		/* REVISIT: This code works only with dedicated chargers!
		 * When support for HOST/HUB chargers is added, don't
		 * forget this.
		 */
		musb_stop(musb);
		/* Regulators off */
		otg_set_suspend(musb->xceiv, 1);
		musb->is_charger = 1;
		if (machine_is_nokia_rx51() && rx51_with_charger_detection())
			rx51_set_wallcharger(1);
	} else {
		/* enable interrupts */
		musb_writeb(musb->mregs, MUSB_INTRUSBE, ctx.intrusbe);

		/* Make sure the communication starts normally */
		r = musb_readb(musb->mregs, MUSB_POWER);
		musb_writeb(musb->mregs, MUSB_POWER,
				r | MUSB_POWER_RESUME);
		msleep(10);
		musb_writeb(musb->mregs, MUSB_POWER,
				r & ~MUSB_POWER_RESUME);
	}

	check_charger = 0;

	return vdat;
}

extern void (*rx51_detect_wallcharger_ptr)(struct work_struct *work);
static void rx51_detect_wallcharger(struct work_struct *work)
{
	if (the_musb)
		musb_charger_detect(the_musb);
}

/*-------------------------------------------------------------------------*/

static inline struct musb *dev_to_musb(struct device *dev)
{
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* usbcore insists dev->driver_data is a "struct hcd *" */
	return hcd_to_musb(dev_get_drvdata(dev));
#else
	return dev_get_drvdata(dev);
#endif
}

/*-------------------------------------------------------------------------*/

#if !defined(CONFIG_USB_TUSB6010) && !defined(CONFIG_BLACKFIN)

/*
 * Load an endpoint's FIFO
 */
void musb_write_fifo(struct musb_hw_ep *hw_ep, u16 len, const u8 *src)
{
	void __iomem *fifo = hw_ep->fifo;

	prefetch((u8 *)src);

	DBG_nonverb(4, "%cX ep%d fifo %p count %d buf %p\n",
			'T', hw_ep->epnum, fifo, len, src);

	/* we can't assume unaligned reads work */
	if (likely((0x01 & (unsigned long) src) == 0)) {
		u16	index = 0;

		/* best case is 32bit-aligned source address */
		if ((0x02 & (unsigned long) src) == 0) {
			if (len >= 4) {
				writesl(fifo, src + index, len >> 2);
				index += len & ~0x03;
			}
			if (len & 0x02) {
				musb_writew(fifo, 0, *(u16 *)&src[index]);
				index += 2;
			}
		} else {
			if (len >= 2) {
				writesw(fifo, src + index, len >> 1);
				index += len & ~0x01;
			}
		}
		if (len & 0x01)
			musb_writeb(fifo, 0, src[index]);
	} else  {
		/* byte aligned */
		writesb(fifo, src, len);
	}
}

/*
 * Unload an endpoint's FIFO
 */
void musb_read_fifo(struct musb_hw_ep *hw_ep, u16 len, u8 *dst)
{
	void __iomem *fifo = hw_ep->fifo;

	DBG_nonverb(4, "%cX ep%d fifo %p count %d buf %p\n",
			'R', hw_ep->epnum, fifo, len, dst);

	/* we can't assume unaligned writes work */
	if (likely((0x01 & (unsigned long) dst) == 0)) {
		u16	index = 0;

		/* best case is 32bit-aligned destination address */
		if ((0x02 & (unsigned long) dst) == 0) {
			if (len >= 4) {
				readsl(fifo, dst, len >> 2);
				index = len & ~0x03;
			}
			if (len & 0x02) {
				*(u16 *)&dst[index] = musb_readw(fifo, 0);
				index += 2;
			}
		} else {
			if (len >= 2) {
				readsw(fifo, dst, len >> 1);
				index = len & ~0x01;
			}
		}
		if (len & 0x01)
			dst[index] = musb_readb(fifo, 0);
	} else  {
		/* byte aligned */
		readsb(fifo, dst, len);
	}
}

#endif	/* normal PIO */


/*-------------------------------------------------------------------------*/

/* for high speed test mode; see USB 2.0 spec 7.1.20 */
static const u8 musb_test_packet[53] = {
	/* implicit SYNC then DATA0 to start */

	/* JKJKJKJK x9 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* JJKKJJKK x8 */
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	/* JJJJKKKK x8 */
	0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
	/* JJJJJJJKKKKKKK x8 */
	0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	/* JJJJJJJK x8 */
	0x7f, 0xbf, 0xdf, 0xef, 0xf7, 0xfb, 0xfd,
	/* JKKKKKKK x10, JK */
	0xfc, 0x7e, 0xbf, 0xdf, 0xef, 0xf7, 0xfb, 0xfd, 0x7e

	/* implicit CRC16 then EOP to end */
};

void musb_load_testpacket(struct musb *musb)
{
	void __iomem	*regs = musb->endpoints[0].regs;

	musb_ep_select(musb->mregs, 0);
	musb_write_fifo(musb->control_ep,
			sizeof(musb_test_packet), musb_test_packet);
	musb_writew(regs, MUSB_CSR0, MUSB_CSR0_TXPKTRDY);
}

/*-------------------------------------------------------------------------*/

const char *otg_state_string(struct musb *musb)
{
	switch (musb->xceiv->state) {
	case OTG_STATE_A_IDLE:		return "a_idle";
	case OTG_STATE_A_WAIT_VRISE:	return "a_wait_vrise";
	case OTG_STATE_A_WAIT_BCON:	return "a_wait_bcon";
	case OTG_STATE_A_HOST:		return "a_host";
	case OTG_STATE_A_SUSPEND:	return "a_suspend";
	case OTG_STATE_A_PERIPHERAL:	return "a_peripheral";
	case OTG_STATE_A_WAIT_VFALL:	return "a_wait_vfall";
	case OTG_STATE_A_VBUS_ERR:	return "a_vbus_err";
	case OTG_STATE_B_IDLE:		return "b_idle";
	case OTG_STATE_B_SRP_INIT:	return "b_srp_init";
	case OTG_STATE_B_PERIPHERAL:	return "b_peripheral";
	case OTG_STATE_B_WAIT_ACON:	return "b_wait_acon";
	case OTG_STATE_B_HOST:		return "b_host";
	default:			return "UNDEFINED";
	}
}

#ifdef	CONFIG_USB_MUSB_OTG

/*
 * See also USB_OTG_1-3.pdf 6.6.5 Timers
 * REVISIT: Are the other timers done in the hardware?
 */
#define TB_ASE0_BRST		100	/* Min 3.125 ms */

/*
 * Handles OTG hnp timeouts, such as b_ase0_brst
 */
void musb_otg_timer_func(unsigned long data)
{
	struct musb	*musb = (struct musb *)data;
	unsigned long	flags;

	spin_lock_irqsave(&musb->lock, flags);
	switch (musb->xceiv->state) {
	case OTG_STATE_B_WAIT_ACON:
		DBG(1, "HNP: b_wait_acon timeout; back to b_peripheral\n");
		musb_g_disconnect(musb);
		musb->xceiv->state = OTG_STATE_B_PERIPHERAL;
		musb->is_active = 0;
		break;
	case OTG_STATE_A_WAIT_BCON:
		DBG(1, "HNP: a_wait_bcon timeout; back to a_host\n");
		musb_hnp_stop(musb);
		break;
	default:
		DBG(1, "HNP: Unhandled mode %s\n", otg_state_string(musb));
	}
	musb->ignore_disconnect = 0;
	spin_unlock_irqrestore(&musb->lock, flags);
}

static DEFINE_TIMER(musb_otg_timer, musb_otg_timer_func, 0, 0);

/*
 * Stops the B-device HNP state. Caller must take care of locking.
 */
void musb_hnp_stop(struct musb *musb)
{
	struct usb_hcd	*hcd = musb_to_hcd(musb);
	void __iomem	*mbase = musb->mregs;
	u8	reg;

	switch (musb->xceiv->state) {
	case OTG_STATE_A_PERIPHERAL:
	case OTG_STATE_A_WAIT_VFALL:
	case OTG_STATE_A_WAIT_BCON:
		DBG(1, "HNP: Switching back to A-host\n");
		musb_g_disconnect(musb);
		musb->xceiv->state = OTG_STATE_A_IDLE;
		MUSB_HST_MODE(musb);
		musb->is_active = 0;
		break;
	case OTG_STATE_B_HOST:
		DBG(1, "HNP: Disabling HR\n");
		hcd->self.is_b_host = 0;
		musb->xceiv->state = OTG_STATE_B_PERIPHERAL;
		MUSB_DEV_MODE(musb);
		reg = musb_readb(mbase, MUSB_POWER);
		reg |= MUSB_POWER_SUSPENDM;
		musb_writeb(mbase, MUSB_POWER, reg);
		/* REVISIT: Start SESSION_REQUEST here? */
		break;
	default:
		DBG(1, "HNP: Stopping in unknown state %s\n",
			otg_state_string(musb));
	}

	/*
	 * When returning to A state after HNP, avoid hub_port_rebounce(),
	 * which cause occasional OPT A "Did not receive reset after connect"
	 * errors.
	 */
	musb->port1_status &=
		~(1 << USB_PORT_FEAT_C_CONNECTION);
}

#endif

/*
 * Interrupt Service Routine to record USB "global" interrupts.
 * Since these do not happen often and signify things of
 * paramount importance, it seems OK to check them individually;
 * the order of the tests is specified in the manual
 *
 * @param musb instance pointer
 * @param int_usb register contents
 * @param devctl
 * @param power
 */

static irqreturn_t musb_stage0_irq(struct musb *musb, u8 int_usb,
				u8 devctl, u8 power)
{
	irqreturn_t handled = IRQ_NONE;
	void __iomem *mbase = musb->mregs;
	u8 r;

        DBG(3, "<== State=%s Power=%02x, DevCtl=%02x, int_usb=0x%x\n",
                otg_state_string(musb), power, devctl, int_usb);

	/* in host mode, the peripheral may issue remote wakeup.
	 * in peripheral mode, the host may resume the link.
	 * spurious RESUME irqs happen too, paired with SUSPEND.
	 */
	if (int_usb & MUSB_INTR_RESUME) {
		handled = IRQ_HANDLED;
		DBG(3, "RESUME (%s)\n", otg_state_string(musb));

		if (devctl & MUSB_DEVCTL_HM) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
			switch (musb->xceiv->state) {
			case OTG_STATE_A_SUSPEND:
				/* remote wakeup?  later, GetPortStatus
				 * will stop RESUME signaling
				 */

				if (power & MUSB_POWER_SUSPENDM) {
					/* spurious */
					musb->int_usb &= ~MUSB_INTR_SUSPEND;
					DBG(2, "Spurious SUSPENDM\n");
					break;
				}

				power &= ~MUSB_POWER_SUSPENDM;
				musb_writeb(mbase, MUSB_POWER,
						power | MUSB_POWER_RESUME);

				musb->port1_status |=
						(USB_PORT_STAT_C_SUSPEND << 16)
						| MUSB_PORT_STAT_RESUME;
				musb->rh_timer = jiffies
						+ msecs_to_jiffies(20);

				musb->xceiv->state = OTG_STATE_A_HOST;
				musb->is_active = 1;
				usb_hcd_resume_root_hub(musb_to_hcd(musb));
				break;
			case OTG_STATE_B_WAIT_ACON:
				musb->xceiv->state = OTG_STATE_B_PERIPHERAL;
				musb->is_active = 1;
				MUSB_DEV_MODE(musb);
				break;
			default:
				WARNING("bogus %s RESUME (%s)\n",
					"host",
					otg_state_string(musb));
			}
#endif
		} else {
			switch (musb->xceiv->state) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
			case OTG_STATE_A_SUSPEND:
				/* possibly DISCONNECT is upcoming */
				musb->xceiv->state = OTG_STATE_A_HOST;
				usb_hcd_resume_root_hub(musb_to_hcd(musb));
				break;
#endif
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
			case OTG_STATE_B_WAIT_ACON:
			case OTG_STATE_B_PERIPHERAL:
				/* disconnect while suspended?  we may
				 * not get a disconnect irq...
				 */
				if ((devctl & MUSB_DEVCTL_VBUS)
						!= (3 << MUSB_DEVCTL_VBUS_SHIFT)
						) {
					musb->int_usb |= MUSB_INTR_DISCONNECT;
					musb->int_usb &= ~MUSB_INTR_SUSPEND;
					break;
				}
				musb_g_resume(musb);
				break;
			case OTG_STATE_B_IDLE:
				musb->int_usb &= ~MUSB_INTR_SUSPEND;
				break;
#endif
			default:
				WARNING("bogus %s RESUME (%s)\n",
					"peripheral",
					otg_state_string(musb));
			}
		}
	}

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* see manual for the order of the tests */
	if (int_usb & MUSB_INTR_SESSREQ) {
		DBG(1, "SESSION_REQUEST (%s)\n", otg_state_string(musb));

		/* IRQ arrives from ID pin sense or (later, if VBUS power
		 * is removed) SRP.  responses are time critical:
		 *  - turn on VBUS (with silicon-specific mechanism)
		 *  - go through A_WAIT_VRISE
		 *  - ... to A_WAIT_BCON.
		 * a_wait_vrise_tmout triggers VBUS_ERROR transitions
		 * NOTE : Spurious SESS_REQ int's detected, which should
		 * be discarded silently.
		 */
		if ((devctl & MUSB_DEVCTL_VBUS)
		    && !(devctl & MUSB_DEVCTL_BDEVICE)) {
			musb_writeb(mbase, MUSB_DEVCTL, MUSB_DEVCTL_SESSION);
			musb->ep0_stage = MUSB_EP0_START;
			musb->xceiv->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
			musb_set_vbus(musb, 1);
		} else {
			DBG(5,"discarding SESSREQ INT: VBUS < SessEnd\n");
		}

		handled = IRQ_HANDLED;
	}

	if (int_usb & MUSB_INTR_VBUSERROR) {
		int	ignore = 0;

		/* During connection as an A-Device, we may see a short
		 * current spikes causing voltage drop, because of cable
		 * and peripheral capacitance combined with vbus draw.
		 * (So: less common with truly self-powered devices, where
		 * vbus doesn't act like a power supply.)
		 *
		 * Such spikes are short; usually less than ~500 usec, max
		 * of ~2 msec.  That is, they're not sustained overcurrent
		 * errors, though they're reported using VBUSERROR irqs.
		 *
		 * Workarounds:  (a) hardware: use self powered devices.
		 * (b) software:  ignore non-repeated VBUS errors.
		 *
		 * REVISIT:  do delays from lots of DEBUG_KERNEL checks
		 * make trouble here, keeping VBUS < 4.4V ?
		 */
		switch (musb->xceiv->state) {
		case OTG_STATE_A_HOST:
			/* recovery is dicey once we've gotten past the
			 * initial stages of enumeration, but if VBUS
			 * stayed ok at the other end of the link, and
			 * another reset is due (at least for high speed,
			 * to redo the chirp etc), it might work OK...
			 */
		case OTG_STATE_A_WAIT_BCON:
		case OTG_STATE_A_WAIT_VRISE:
			if (musb->vbuserr_retry) {
				musb->vbuserr_retry--;
				ignore = 1;
				devctl |= MUSB_DEVCTL_SESSION;
				musb_writeb(mbase, MUSB_DEVCTL, devctl);
			} else {
				musb->port1_status |=
					  (1 << USB_PORT_FEAT_OVER_CURRENT)
					| (1 << USB_PORT_FEAT_C_OVER_CURRENT);
			}
			break;
		default:
			break;
		}

		DBG(1, "VBUS_ERROR in %s (%02x, %s), retry #%d, port1 %08x\n",
				otg_state_string(musb),
				devctl,
				({ char *s;
				switch (devctl & MUSB_DEVCTL_VBUS) {
				case 0 << MUSB_DEVCTL_VBUS_SHIFT:
					s = "<SessEnd"; break;
				case 1 << MUSB_DEVCTL_VBUS_SHIFT:
					s = "<AValid"; break;
				case 2 << MUSB_DEVCTL_VBUS_SHIFT:
					s = "<VBusValid"; break;
				/* case 3 << MUSB_DEVCTL_VBUS_SHIFT: */
				default:
					s = "VALID"; break;
				}; s; }),
				VBUSERR_RETRY_COUNT - musb->vbuserr_retry,
				musb->port1_status);

		/* go through A_WAIT_VFALL then start a new session */
		if (!ignore)
			musb_set_vbus(musb, 0);
		handled = IRQ_HANDLED;
	}

	if (int_usb & MUSB_INTR_SUSPEND) {
		DBG(1, "SUSPEND (%s) devctl %02x power %02x\n",
				otg_state_string(musb), devctl, power);
		handled = IRQ_HANDLED;

		switch (musb->xceiv->state) {
#ifdef	CONFIG_USB_MUSB_OTG
		case OTG_STATE_A_PERIPHERAL:
			/*
			 * We cannot stop HNP here, devctl BDEVICE might be
			 * still set.
			 */
			break;
#endif
		case OTG_STATE_B_IDLE:
			if (!musb->is_active)
				break;
		case OTG_STATE_B_PERIPHERAL:
			musb_g_suspend(musb);
			musb->is_active = is_otg_enabled(musb)
					&& musb->xceiv->gadget->b_hnp_enable;
			if (musb->is_active) {
#ifdef	CONFIG_USB_MUSB_OTG
				musb->xceiv->state = OTG_STATE_B_WAIT_ACON;
				DBG(1, "HNP: Setting timer for b_ase0_brst\n");
				musb_otg_timer.data = (unsigned long)musb;
				mod_timer(&musb_otg_timer, jiffies
					+ msecs_to_jiffies(TB_ASE0_BRST));
#endif
			}
			break;
		case OTG_STATE_A_WAIT_BCON:
			if (musb->a_wait_bcon != 0)
				musb_platform_try_idle(musb, jiffies
					+ msecs_to_jiffies(musb->a_wait_bcon));
			break;
		case OTG_STATE_A_HOST:
			musb->xceiv->state = OTG_STATE_A_SUSPEND;
			musb->is_active = is_otg_enabled(musb)
					&& musb->xceiv->host->b_hnp_enable;
			break;
		case OTG_STATE_B_HOST:
			/* Transition to B_PERIPHERAL, see 6.8.2.6 p 44 */
			DBG(1, "REVISIT: SUSPEND as B_HOST\n");
			break;
		default:
			/* "should not happen" */
			musb->is_active = 0;
			break;
		}
	}

	if (int_usb & MUSB_INTR_CONNECT) {
		struct usb_hcd *hcd = musb_to_hcd(musb);

		handled = IRQ_HANDLED;
		musb->is_active = 1;
		set_bit(HCD_FLAG_SAW_IRQ, &hcd->flags);

		musb->ep0_stage = MUSB_EP0_START;

#ifdef CONFIG_USB_MUSB_OTG
		/* flush endpoints when transitioning from Device Mode */
		if (is_peripheral_active(musb)) {
			/* REVISIT HNP; just force disconnect */
		}
		musb_writew(mbase, MUSB_INTRTXE, musb->epmask);
		musb_writew(mbase, MUSB_INTRRXE, musb->epmask & 0xfffe);
		musb_writeb(mbase, MUSB_INTRUSBE, 0xf7);
#endif
		musb->port1_status &= ~(USB_PORT_STAT_LOW_SPEED
					|USB_PORT_STAT_HIGH_SPEED
					|USB_PORT_STAT_ENABLE
					);
		musb->port1_status |= USB_PORT_STAT_CONNECTION
					|(USB_PORT_STAT_C_CONNECTION << 16);

		/* high vs full speed is just a guess until after reset */
		if (devctl & MUSB_DEVCTL_LSDEV)
			musb->port1_status |= USB_PORT_STAT_LOW_SPEED;

		if (hcd->status_urb)
			usb_hcd_poll_rh_status(hcd);
		else
			usb_hcd_resume_root_hub(hcd);

		MUSB_HST_MODE(musb);

		/* indicate new connection to OTG machine */
		switch (musb->xceiv->state) {
		case OTG_STATE_B_PERIPHERAL:
			if (int_usb & MUSB_INTR_SUSPEND) {
				DBG(1, "HNP: SUSPEND+CONNECT, now b_host\n");
				musb->xceiv->state = OTG_STATE_B_HOST;
				hcd->self.is_b_host = 1;
				int_usb &= ~MUSB_INTR_SUSPEND;
			} else
				DBG(1, "CONNECT as b_peripheral???\n");
			break;
		case OTG_STATE_B_WAIT_ACON:
			DBG(1, "HNP: Waiting to switch to b_host state\n");
			musb->xceiv->state = OTG_STATE_B_HOST;
			hcd->self.is_b_host = 1;
			break;
		default:
			if ((devctl & MUSB_DEVCTL_VBUS)
					== (3 << MUSB_DEVCTL_VBUS_SHIFT)) {
				musb->xceiv->state = OTG_STATE_A_HOST;
				hcd->self.is_b_host = 0;
			}
			break;
		}
		DBG(1, "CONNECT (%s) devctl %02x\n",
				otg_state_string(musb), devctl);
	}
#endif	/* CONFIG_USB_MUSB_HDRC_HCD */

	if ((int_usb & MUSB_INTR_DISCONNECT) && !musb->ignore_disconnect) {
		DBG(1, "DISCONNECT (%s) as %s, devctl %02x\n",
				otg_state_string(musb),
				MUSB_MODE(musb), devctl);
		handled = IRQ_HANDLED;

		switch (musb->xceiv->state) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
		case OTG_STATE_A_HOST:
		case OTG_STATE_A_SUSPEND:
			usb_hcd_resume_root_hub(musb_to_hcd(musb));
			musb_root_disconnect(musb);
			if (musb->a_wait_bcon != 0 && is_otg_enabled(musb))
				musb_platform_try_idle(musb, jiffies
					+ msecs_to_jiffies(musb->a_wait_bcon));
			break;
#endif	/* HOST */
#ifdef CONFIG_USB_MUSB_OTG
		case OTG_STATE_B_HOST:
			musb_hnp_stop(musb);
			break;
		case OTG_STATE_A_PERIPHERAL:
			musb_hnp_stop(musb);
			musb_root_disconnect(musb);
			/* FALLTHROUGH */
		case OTG_STATE_B_WAIT_ACON:
			/* FALLTHROUGH */
#endif	/* OTG */
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
		case OTG_STATE_B_PERIPHERAL:
		case OTG_STATE_B_IDLE:
			/* Workaround for a problem of Vbus quickly dropping
			 * during Certification tests.
			 *
			 * Undo the workaround on disconnect
			 */

			/* Disable suspend so we can write to ULPI */
			r = musb_readb(musb->mregs, MUSB_POWER);
			musb_writeb(musb->mregs, MUSB_POWER,
						r & ~MUSB_POWER_ENSUSPEND);
			musb_ulpi_writeb(musb->mregs,
						ISP1704_USB_INTFALL, 0x1f);
			musb_ulpi_writeb(musb->mregs,
						ISP1704_USB_INTRISE, 0x1f);
			musb_writeb(musb->mregs, MUSB_POWER,
						r | MUSB_POWER_ENSUSPEND);

			musb_g_disconnect(musb);
			/** UGLY UGLY HACK: Windows problems with multiple
			 * configurations.
			 *
			 * This is necessary to notify gadget driver this was
			 * a physical disconnection and not only a port reset.
			 */
			if (musb->gadget_driver->vbus_disconnect)
				musb->gadget_driver->vbus_disconnect(&musb->g);

			break;
#endif	/* GADGET */
		default:
			WARNING("unhandled DISCONNECT transition (%s)\n",
				otg_state_string(musb));
			break;
		}
	}

	/* mentor saves a bit: bus reset and babble share the same irq.
	 * only host sees babble; only peripheral sees bus reset.
	 */
	if (int_usb & MUSB_INTR_RESET) {
		handled = IRQ_HANDLED;
		if (is_host_capable() && (devctl & MUSB_DEVCTL_HM) != 0) {
			/*
			 * Looks like non-HS BABBLE can be ignored, but
			 * HS BABBLE is an error condition. For HS the solution
			 * is to avoid babble in the first place and fix what
			 * caused BABBLE. When HS BABBLE happens we can only
			 * stop the session.
			 */
			if (devctl & (MUSB_DEVCTL_FSDEV | MUSB_DEVCTL_LSDEV))
				DBG(1, "BABBLE devctl: %02x\n", devctl);
			else {
				ERR("Stopping host session -- babble\n");
				musb_writeb(musb->mregs, MUSB_DEVCTL, 0);
			}
		} else if (is_peripheral_capable()) {
			DBG(1, "BUS RESET as %s\n", otg_state_string(musb));
			switch (musb->xceiv->state) {
#ifdef CONFIG_USB_OTG
			case OTG_STATE_A_SUSPEND:
				/* We need to ignore disconnect on suspend
				 * otherwise tusb 2.0 won't reconnect after a
				 * power cycle, which breaks otg compliance.
				 */
				musb->ignore_disconnect = 1;
				musb_g_reset(musb);
				/* FALLTHROUGH */
			case OTG_STATE_A_WAIT_BCON:	/* OPT TD.4.7-900ms */
				DBG(1, "HNP: Setting timer as %s\n",
						otg_state_string(musb));
				musb_otg_timer.data = (unsigned long)musb;
				mod_timer(&musb_otg_timer, jiffies
					+ msecs_to_jiffies(100));
				break;
			case OTG_STATE_A_PERIPHERAL:
				musb_hnp_stop(musb);
				break;
			case OTG_STATE_B_WAIT_ACON:
				DBG(1, "HNP: RESET (%s), to b_peripheral\n",
					otg_state_string(musb));
				musb->xceiv->state = OTG_STATE_B_PERIPHERAL;
				musb_g_reset(musb);
				break;
#endif
			case OTG_STATE_B_IDLE:
				/* Workaround the charger detection problems */
				if ((devctl & MUSB_DEVCTL_VBUS)
					!= (3 << MUSB_DEVCTL_VBUS_SHIFT))
					break;
				if (check_charger)
					break;
				musb->xceiv->state = OTG_STATE_B_PERIPHERAL;
				/* FALLTHROUGH */
			case OTG_STATE_B_PERIPHERAL:
			/* Workaround for a problem of Vbus quickly dropping
			 * during Certification tests.
			 *
			 * The guess is that vbus drops due to the circuitry
			 * for overcurrent protection and that makes transceiver
			 * think VBUS is not valid anymore. Transceiver will
			 * then send an RXCMD to PHY which will cause it to
			 * disconnect from the bus even though we disable the
			 * DISCONNECT IRQ
			 */
				musb_ulpi_writeb(musb->mregs,
						 ISP1704_USB_INTFALL, 0x1d);
				musb_ulpi_writeb(musb->mregs,
						 ISP1704_USB_INTRISE, 0x1d);

				musb_g_reset(musb);
				break;
			default:
				DBG(1, "Unhandled BUS RESET as %s\n",
					otg_state_string(musb));
			}
		}
	}

#if 0
/* REVISIT ... this would be for multiplexing periodic endpoints, or
 * supporting transfer phasing to prevent exceeding ISO bandwidth
 * limits of a given frame or microframe.
 *
 * It's not needed for peripheral side, which dedicates endpoints;
 * though it _might_ use SOF irqs for other purposes.
 *
 * And it's not currently needed for host side, which also dedicates
 * endpoints, relies on TX/RX interval registers, and isn't claimed
 * to support ISO transfers yet.
 */
	if (int_usb & MUSB_INTR_SOF) {
		void __iomem *mbase = musb->mregs;
		struct musb_hw_ep	*ep;
		u8 epnum;
		u16 frame;

		DBG(6, "START_OF_FRAME\n");
		handled = IRQ_HANDLED;

		/* start any periodic Tx transfers waiting for current frame */
		frame = musb_readw(mbase, MUSB_FRAME);
		ep = musb->endpoints;
		for (epnum = 1; (epnum < musb->nr_endpoints)
					&& (musb->epmask >= (1 << epnum));
				epnum++, ep++) {
			/*
			 * FIXME handle framecounter wraps (12 bits)
			 * eliminate duplicated StartUrb logic
			 */
			if (ep->dwWaitFrame >= frame) {
				ep->dwWaitFrame = 0;
				pr_debug("SOF --> periodic TX%s on %d\n",
					ep->tx_channel ? " DMA" : "",
					epnum);
				if (!ep->tx_channel)
					musb_h_tx_start(musb, epnum);
				else
					cppi_hostdma_start(musb, epnum);
			}
		}		/* end of for loop */
	}
#endif

	schedule_work(&musb->irq_work);

	return handled;
}

/*-------------------------------------------------------------------------*/

/*
* Program the HDRC to start (enable interrupts, dma, etc.).
*/
void musb_start(struct musb *musb)
{
	void __iomem	*regs = musb->mregs;
	u8		devctl = musb_readb(regs, MUSB_DEVCTL);
	u8		power;

	DBG(2, "<== devctl %02x\n", devctl);

	/* Ensure the clocks are on */
	if (musb->set_clock)
		musb->set_clock(musb->clock, 1);
	else
		clk_enable(musb->clock);

	/*  Set INT enable registers, enable interrupts */
	musb_writew(regs, MUSB_INTRTXE, musb->epmask);
	musb_writew(regs, MUSB_INTRRXE, musb->epmask & 0xfffe);
	musb_writeb(regs, MUSB_INTRUSBE, 0xf7);

	musb_writeb(regs, MUSB_TESTMODE, 0);

	power = MUSB_POWER_ISOUPDATE | MUSB_POWER_SOFTCONN
		| MUSB_POWER_HSENAB;

	/* ENSUSPEND wedges tusb */
	if (musb->suspendm)
		power |= MUSB_POWER_ENSUSPEND;

	/* put into basic highspeed mode and start session */
	musb_writeb(regs, MUSB_POWER, power);

	musb->is_active = 0;
	devctl = musb_readb(regs, MUSB_DEVCTL);
	devctl &= ~MUSB_DEVCTL_SESSION;

	if (is_otg_enabled(musb)) {
		/* session started after:
		 * (a) ID-grounded irq, host mode;
		 * (b) vbus present/connect IRQ, peripheral mode;
		 * (c) peripheral initiates, using SRP
		 */
		if ((devctl & MUSB_DEVCTL_VBUS) == MUSB_DEVCTL_VBUS)
			musb->is_active = 1;
		else
			devctl |= MUSB_DEVCTL_SESSION;

	} else if (is_host_enabled(musb)) {
		/* assume ID pin is hard-wired to ground */
		devctl |= MUSB_DEVCTL_SESSION;

	} else /* peripheral is enabled */ {
		if ((devctl & MUSB_DEVCTL_VBUS) == MUSB_DEVCTL_VBUS)
			musb->is_active = 1;
	}
	musb_platform_enable(musb);
	musb_writeb(regs, MUSB_DEVCTL, devctl);
}


static void musb_generic_disable(struct musb *musb)
{
	void __iomem	*mbase = musb->mregs;
	u16	temp;

	/* Clocks need to be turned on with OFF-mode */
	if (musb->clock) {
		if (musb->set_clock)
			musb->set_clock(musb->clock, 1);
		else
			clk_enable(musb->clock);
	}

	/* disable interrupts */
	musb_writeb(mbase, MUSB_INTRUSBE, 0);
	musb_writew(mbase, MUSB_INTRTXE, 0);
	musb_writew(mbase, MUSB_INTRRXE, 0);

	/* off */
	musb_writeb(mbase, MUSB_DEVCTL, 0);

	/*  flush pending interrupts */
	temp = musb_readb(mbase, MUSB_INTRUSB);
	temp = musb_readw(mbase, MUSB_INTRTX);
	temp = musb_readw(mbase, MUSB_INTRRX);

}

extern void (*musb_emergency_stop_ptr)(void);
static void musb_emergency_stop(void)
{
	if (!the_musb)
		return;

	musb_stop(the_musb);
}

/*
 * Make the HDRC stop (disable interrupts, etc.);
 * reversible by musb_start
 * called on gadget driver unregister
 * with controller locked, irqs blocked
 * acts as a NOP unless some role activated the hardware
 */
void musb_stop(struct musb *musb)
{
	/* stop IRQs, timers, ... */
	musb_platform_disable(musb);
	musb_generic_disable(musb);
	DBG(3, "HDRC disabled\n");

	/* FIXME
	 *  - mark host and/or peripheral drivers unusable/inactive
	 *  - disable DMA (and enable it in HdrcStart)
	 *  - make sure we can musb_start() after musb_stop(); with
	 *    OTG mode, gadget driver module rmmod/modprobe cycles that
	 *  - ...
	 */
	musb_platform_try_idle(musb, 0);
}

static void musb_shutdown(struct platform_device *pdev)
{
	struct musb	*musb = dev_to_musb(&pdev->dev);
	unsigned long	flags;

	spin_lock_irqsave(&musb->lock, flags);
	musb_platform_disable(musb);
	musb_generic_disable(musb);
	if (musb->clock) {
		clk_put(musb->clock);
		musb->clock = NULL;
	}
	spin_unlock_irqrestore(&musb->lock, flags);

	/* FIXME power down */
}


/*-------------------------------------------------------------------------*/

/*
 * The silicon either has hard-wired endpoint configurations, or else
 * "dynamic fifo" sizing.  The driver has support for both, though at this
 * writing only the dynamic sizing is very well tested.   Since we switched
 * away from compile-time hardware parameters, we can no longer rely on
 * dead code elimination to leave only the relevant one in the object file.
 *
 * We don't currently use dynamic fifo setup capability to do anything
 * more than selecting one of a bunch of predefined configurations.
 */
#if defined(CONFIG_USB_TUSB6010) || \
	defined(CONFIG_ARCH_OMAP2430) || defined(CONFIG_ARCH_OMAP34XX)
static ushort __initdata fifo_mode = 4;
#else
static ushort __initdata fifo_mode = 2;
#endif

/* "modprobe ... fifo_mode=1" etc */
module_param(fifo_mode, ushort, 0);
MODULE_PARM_DESC(fifo_mode, "initial endpoint configuration");


enum fifo_style { FIFO_RXTX, FIFO_TX, FIFO_RX } __attribute__ ((packed));
enum buf_mode { BUF_SINGLE, BUF_DOUBLE } __attribute__ ((packed));

struct fifo_cfg {
	u8		hw_ep_num;
	enum fifo_style	style;
	enum buf_mode	mode;
	u16		maxpacket;
};

/*
 * tables defining fifo_mode values.  define more if you like.
 * for host side, make sure both halves of ep1 are set up.
 */

/* mode 0 - fits in 2KB */
static struct fifo_cfg __initdata mode_0_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_RXTX, .maxpacket = 512, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 1 - fits in 4KB */
static struct fifo_cfg __initdata mode_1_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 2, .style = FIFO_RXTX, .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 2 - fits in 4KB */
static struct fifo_cfg __initdata mode_2_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 3 - fits in 4KB */
static struct fifo_cfg __initdata mode_3_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 2, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 4 - fits in 16KB */
static struct fifo_cfg __initdata mode_4_cfg[] = {
{ .hw_ep_num =  1, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  1, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  2, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  2, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  3, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  3, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  4, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  4, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  5, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  5, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  6, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  6, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  7, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  7, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num =  8, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  8, .style = FIFO_RX,   .maxpacket = 64, },
{ .hw_ep_num =  9, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  9, .style = FIFO_RX,   .maxpacket = 64, },
{ .hw_ep_num = 10, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 10, .style = FIFO_RX,   .maxpacket = 64, },
{ .hw_ep_num = 11, .style = FIFO_TX,   .maxpacket = 256, },
{ .hw_ep_num = 11, .style = FIFO_RX,   .maxpacket = 256, },
{ .hw_ep_num = 12, .style = FIFO_TX,   .maxpacket = 256, },
{ .hw_ep_num = 12, .style = FIFO_RX,   .maxpacket = 256, },
{ .hw_ep_num = 13, .style = FIFO_TX,   .maxpacket = 256, },
{ .hw_ep_num = 13, .style = FIFO_RX,   .maxpacket = 4096, },
{ .hw_ep_num = 14, .style = FIFO_RXTX, .maxpacket = 1024, },
{ .hw_ep_num = 15, .style = FIFO_RXTX, .maxpacket = 1024, },
};

/* mode 5 - fits in 16KB */
static struct fifo_cfg __initdata mode_5_cfg[] = {
/* phonet or mass storage */
{ .hw_ep_num =  1, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  1, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_SINGLE, },

/* obex 1 */
{ .hw_ep_num =  2, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  2, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_SINGLE, },

/* obex 2 */
{ .hw_ep_num =  3, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  3, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_SINGLE, },

/* acm 1 */
{ .hw_ep_num =  4, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  4, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  5, .style = FIFO_TX,   .maxpacket = 16, },

/* ecm */
{ .hw_ep_num =  6, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  5, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_SINGLE, },
{ .hw_ep_num =  7, .style = FIFO_TX,   .maxpacket = 16, },

/* extras */
{ .hw_ep_num =  8, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  6, .style = FIFO_RX,   .maxpacket = 512, },

{ .hw_ep_num =  9, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num =  7, .style = FIFO_RX,   .maxpacket = 512, },

{ .hw_ep_num =  10, .style = FIFO_TX,  .maxpacket = 512, },
{ .hw_ep_num =  8, .style = FIFO_RX,   .maxpacket = 512, },

{ .hw_ep_num =  11, .style = FIFO_TX,  .maxpacket = 512, },
{ .hw_ep_num =  9, .style = FIFO_RX,   .maxpacket = 512, },
};

/*
 * configure a fifo; for non-shared endpoints, this may be called
 * once for a tx fifo and once for an rx fifo.
 *
 * returns negative errno or offset for next fifo.
 */
static int __init
fifo_setup(struct musb *musb, struct musb_hw_ep  *hw_ep,
		const struct fifo_cfg *cfg, u16 offset)
{
	void __iomem	*mbase = musb->mregs;
	int	size = 0;
	u16	maxpacket = cfg->maxpacket;
	u16	c_off = offset >> 3;
	u8	c_size;

	/* expect hw_ep has already been zero-initialized */

	size = ffs(max(maxpacket, (u16) 8)) - 1;
	maxpacket = 1 << size;

	c_size = size - 3;
	if (cfg->mode == BUF_DOUBLE) {
		if ((offset + (maxpacket << 1)) >
				(1 << (musb->config->ram_bits + 2)))
			return -EMSGSIZE;
		c_size |= MUSB_FIFOSZ_DPB;
	} else {
		if ((offset + maxpacket) > (1 << (musb->config->ram_bits + 2)))
			return -EMSGSIZE;
	}

	/* configure the FIFO */
	musb_writeb(mbase, MUSB_INDEX, hw_ep->epnum);

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* EP0 reserved endpoint for control, bidirectional;
	 * EP1 reserved for bulk, two unidirection halves.
	 */
	if (hw_ep->epnum == 1)
		musb->bulk_ep = hw_ep;
	/* REVISIT error check:  be sure ep0 can both rx and tx ... */
#endif
	switch (cfg->style) {
	case FIFO_TX:
		musb_write_txfifosz(mbase, c_size);
		musb_write_txfifoadd(mbase, c_off);
		hw_ep->tx_double_buffered = !!(c_size & MUSB_FIFOSZ_DPB);
		hw_ep->max_packet_sz_tx = maxpacket;
		ctx.txfifosz[hw_ep->epnum] = c_size;
		ctx.txfifoadd[hw_ep->epnum] = c_off;
		break;
	case FIFO_RX:
		musb_write_rxfifosz(mbase, c_size);
		musb_write_rxfifoadd(mbase, c_off);
		hw_ep->rx_double_buffered = !!(c_size & MUSB_FIFOSZ_DPB);
		hw_ep->max_packet_sz_rx = maxpacket;
		ctx.rxfifosz[hw_ep->epnum] = c_size;
		ctx.rxfifoadd[hw_ep->epnum] = c_off;
		break;
	case FIFO_RXTX:
		musb_write_txfifosz(mbase, c_size);
		musb_write_txfifoadd(mbase, c_off);
		hw_ep->rx_double_buffered = !!(c_size & MUSB_FIFOSZ_DPB);
		hw_ep->max_packet_sz_rx = maxpacket;

		musb_write_rxfifosz(mbase, c_size);
		musb_write_rxfifoadd(mbase, c_off);
		hw_ep->tx_double_buffered = hw_ep->rx_double_buffered;
		hw_ep->max_packet_sz_tx = maxpacket;

		/* Save the context of endpoints. */
		ctx.rxfifosz[hw_ep->epnum] = c_size;
		ctx.txfifosz[hw_ep->epnum] = c_size;
		ctx.txfifoadd[hw_ep->epnum] = c_off;
		ctx.rxfifoadd[hw_ep->epnum] = c_off;

		hw_ep->is_shared_fifo = true;
		break;
	}

	/* NOTE rx and tx endpoint irqs aren't managed separately,
	 * which happens to be ok
	 */
	musb->epmask |= (1 << hw_ep->epnum);

	return offset + (maxpacket << ((c_size & MUSB_FIFOSZ_DPB) ? 1 : 0));
}

static struct fifo_cfg __initdata ep0_cfg = {
	.style = FIFO_RXTX, .maxpacket = 64,
};

static int __init ep_config_from_table(struct musb *musb)
{
	const struct fifo_cfg	*cfg;
	unsigned		i, n;
	int			offset;
	struct musb_hw_ep	*hw_ep = musb->endpoints;

	if (machine_is_nokia_rx51())
		fifo_mode = 5;

	switch (fifo_mode) {
	default:
		fifo_mode = 0;
		/* FALLTHROUGH */
	case 0:
		cfg = mode_0_cfg;
		n = ARRAY_SIZE(mode_0_cfg);
		break;
	case 1:
		cfg = mode_1_cfg;
		n = ARRAY_SIZE(mode_1_cfg);
		break;
	case 2:
		cfg = mode_2_cfg;
		n = ARRAY_SIZE(mode_2_cfg);
		break;
	case 3:
		cfg = mode_3_cfg;
		n = ARRAY_SIZE(mode_3_cfg);
		break;
	case 4:
		cfg = mode_4_cfg;
		n = ARRAY_SIZE(mode_4_cfg);
		break;
	case 5:
		cfg = mode_5_cfg;
		n = ARRAY_SIZE(mode_5_cfg);
		break;
	}

	printk(KERN_DEBUG "%s: setup fifo_mode %d\n",
			musb_driver_name, fifo_mode);


	offset = fifo_setup(musb, hw_ep, &ep0_cfg, 0);
	/* assert(offset > 0) */

	/* NOTE:  for RTL versions >= 1.400 EPINFO and RAMINFO would
	 * be better than static musb->config->num_eps and DYN_FIFO_SIZE...
	 */

	for (i = 0; i < n; i++) {
		u8	epn = cfg->hw_ep_num;

		if (epn >= musb->config->num_eps) {
			pr_debug("%s: invalid ep %d\n",
					musb_driver_name, epn);
			return -EINVAL;
		}
		offset = fifo_setup(musb, hw_ep + epn, cfg++, offset);
		if (offset < 0) {
			pr_debug("%s: mem overrun, ep %d\n",
					musb_driver_name, epn);
			return -EINVAL;
		}
		epn++;
		musb->nr_endpoints = max(epn, musb->nr_endpoints);
	}

	printk(KERN_DEBUG "%s: %d/%d max ep, %d/%d memory\n",
			musb_driver_name,
			n + 1, musb->config->num_eps * 2 - 1,
			offset, (1 << (musb->config->ram_bits + 2)));

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (!musb->bulk_ep) {
		pr_debug("%s: missing bulk\n", musb_driver_name);
		return -EINVAL;
	}
#endif

	return 0;
}


/*
 * ep_config_from_hw - when MUSB_C_DYNFIFO_DEF is false
 * @param musb the controller
 */
static int __init ep_config_from_hw(struct musb *musb)
{
	u8 epnum = 0;
	struct musb_hw_ep *hw_ep;
	void *mbase = musb->mregs;
	int ret = 0;

	DBG(2, "<== static silicon ep config\n");

	/* FIXME pick up ep0 maxpacket size */

	for (epnum = 1; epnum < musb->config->num_eps; epnum++) {
		musb_ep_select(mbase, epnum);
		hw_ep = musb->endpoints + epnum;

		ret = musb_read_fifosize(musb, hw_ep, epnum);
		if (ret < 0)
			break;

		/* FIXME set up hw_ep->{rx,tx}_double_buffered */

#ifdef CONFIG_USB_MUSB_HDRC_HCD
		/* pick an RX/TX endpoint for bulk */
		if (hw_ep->max_packet_sz_tx < 512
				|| hw_ep->max_packet_sz_rx < 512)
			continue;

		/* REVISIT:  this algorithm is lazy, we should at least
		 * try to pick a double buffered endpoint.
		 */
		if (musb->bulk_ep)
			continue;
		musb->bulk_ep = hw_ep;
#endif
	}

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (!musb->bulk_ep) {
		pr_debug("%s: missing bulk\n", musb_driver_name);
		return -EINVAL;
	}
#endif

	return 0;
}

enum { MUSB_CONTROLLER_MHDRC, MUSB_CONTROLLER_HDRC, };

/* Initialize MUSB (M)HDRC part of the USB hardware subsystem;
 * configure endpoints, or take their config from silicon
 */
static int __init musb_core_init(u16 musb_type, struct musb *musb)
{
#ifdef MUSB_AHB_ID
	u32 data;
#endif
	u8 reg;
	char *type;
	u16 hwvers, rev_major, rev_minor;
	char aInfo[78], aRevision[32], aDate[12];
	void __iomem	*mbase = musb->mregs;
	int		status = 0;
	int		i;

	/* log core options (read using indexed model) */
	musb_ep_select(mbase, 0);
	reg = musb_read_configdata(mbase);

	strcpy(aInfo, (reg & MUSB_CONFIGDATA_UTMIDW) ? "UTMI-16" : "UTMI-8");
	if (reg & MUSB_CONFIGDATA_DYNFIFO)
		strcat(aInfo, ", dyn FIFOs");
	if (reg & MUSB_CONFIGDATA_MPRXE) {
		strcat(aInfo, ", bulk combine");
#ifdef C_MP_RX
		musb->bulk_combine = true;
#else
		strcat(aInfo, " (X)");		/* no driver support */
#endif
	}
	if (reg & MUSB_CONFIGDATA_MPTXE) {
		strcat(aInfo, ", bulk split");
#ifdef C_MP_TX
		musb->bulk_split = true;
#else
		strcat(aInfo, " (X)");		/* no driver support */
#endif
	}
	if (reg & MUSB_CONFIGDATA_HBRXE) {
		strcat(aInfo, ", HB-ISO Rx");
		strcat(aInfo, " (X)");		/* no driver support */
	}
	if (reg & MUSB_CONFIGDATA_HBTXE) {
		strcat(aInfo, ", HB-ISO Tx");
		strcat(aInfo, " (X)");		/* no driver support */
	}
	if (reg & MUSB_CONFIGDATA_SOFTCONE)
		strcat(aInfo, ", SoftConn");

	printk(KERN_DEBUG "%s: ConfigData=0x%02x (%s)\n",
			musb_driver_name, reg, aInfo);

#ifdef MUSB_AHB_ID
	data = musb_readl(mbase, 0x404);
	sprintf(aDate, "%04d-%02x-%02x", (data & 0xffff),
		(data >> 16) & 0xff, (data >> 24) & 0xff);
	/* FIXME ID2 and ID3 are unused */
	data = musb_readl(mbase, 0x408);
	printk(KERN_DEBUG "ID2=%lx\n", (long unsigned)data);
	data = musb_readl(mbase, 0x40c);
	printk(KERN_DEBUG "ID3=%lx\n", (long unsigned)data);
	reg = musb_readb(mbase, 0x400);
	musb_type = ('M' == reg) ? MUSB_CONTROLLER_MHDRC : MUSB_CONTROLLER_HDRC;
#else
	aDate[0] = 0;
#endif
	if (MUSB_CONTROLLER_MHDRC == musb_type) {
		musb->is_multipoint = 1;
		type = "M";
	} else {
		musb->is_multipoint = 0;
		type = "";
#ifdef CONFIG_USB_MUSB_HDRC_HCD
#ifndef	CONFIG_USB_OTG_BLACKLIST_HUB
		printk(KERN_ERR
			"%s: kernel must blacklist external hubs\n",
			musb_driver_name);
#endif
#endif
	}

	/* log release info */
	hwvers = musb_read_hwvers(mbase);
	rev_major = (hwvers >> 10) & 0x1f;
	rev_minor = hwvers & 0x3ff;
	snprintf(aRevision, 32, "%d.%d%s", rev_major,
		rev_minor, (hwvers & 0x8000) ? "RC" : "");
	printk(KERN_DEBUG "%s: %sHDRC RTL version %s %s\n",
			musb_driver_name, type, aRevision, aDate);

	/* configure ep0 */
	musb_configure_ep0(musb);

	/* discover endpoint configuration */
	musb->nr_endpoints = 1;
	musb->epmask = 1;

	if (reg & MUSB_CONFIGDATA_DYNFIFO) {
		if (musb->config->dyn_fifo)
			status = ep_config_from_table(musb);
		else {
			ERR("reconfigure software for Dynamic FIFOs\n");
			status = -ENODEV;
		}
	} else {
		if (!musb->config->dyn_fifo)
			status = ep_config_from_hw(musb);
		else {
			ERR("reconfigure software for static FIFOs\n");
			return -ENODEV;
		}
	}

	if (status < 0)
		return status;

	/* finish init, and print endpoint config */
	for (i = 0; i < musb->nr_endpoints; i++) {
		struct musb_hw_ep	*hw_ep = musb->endpoints + i;

		hw_ep->fifo = MUSB_FIFO_OFFSET(i) + mbase;
#ifdef CONFIG_USB_TUSB6010
		hw_ep->fifo_async = musb->async + 0x400 + MUSB_FIFO_OFFSET(i);
		hw_ep->fifo_sync = musb->sync + 0x400 + MUSB_FIFO_OFFSET(i);
		hw_ep->fifo_sync_va =
			musb->sync_va + 0x400 + MUSB_FIFO_OFFSET(i);

		if (i == 0)
			hw_ep->conf = mbase - 0x400 + TUSB_EP0_CONF;
		else
			hw_ep->conf = mbase + 0x400 + (((i - 1) & 0xf) << 2);
#endif

		hw_ep->regs = MUSB_EP_OFFSET(i, 0) + mbase;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
		/* init list of in and out qhs */
		INIT_LIST_HEAD(&hw_ep->in_list);
		INIT_LIST_HEAD(&hw_ep->out_list);
		hw_ep->target_regs = musb_read_target_reg_base(i, mbase);
		hw_ep->rx_reinit = 1;
		hw_ep->tx_reinit = 1;
#endif

		if (hw_ep->max_packet_sz_tx) {
			printk(KERN_DEBUG
				"%s: hw_ep %d%s, %smax %d\n",
				musb_driver_name, i,
				hw_ep->is_shared_fifo ? "shared" : "tx",
				hw_ep->tx_double_buffered
					? "doublebuffer, " : "",
				hw_ep->max_packet_sz_tx);
		}
		if (hw_ep->max_packet_sz_rx && !hw_ep->is_shared_fifo) {
			printk(KERN_DEBUG
				"%s: hw_ep %d%s, %smax %d\n",
				musb_driver_name, i,
				"rx",
				hw_ep->rx_double_buffered
					? "doublebuffer, " : "",
				hw_ep->max_packet_sz_rx);
		}
		if (!(hw_ep->max_packet_sz_tx || hw_ep->max_packet_sz_rx))
			DBG(1, "hw_ep %d not configured\n", i);
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

#if defined(CONFIG_ARCH_OMAP2430) || defined(CONFIG_ARCH_OMAP3430)

static irqreturn_t generic_interrupt(int irq, void *__hci)
{
	unsigned long	flags;
	irqreturn_t	retval = IRQ_NONE;
	struct musb	*musb = __hci;

	spin_lock_irqsave(&musb->lock, flags);

	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);

	while (musb->int_usb || musb->int_tx || musb->int_rx)
		retval |= musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	/* REVISIT we sometimes get spurious IRQs on g_ep0
	 * not clear why...
	 */
	if (retval != IRQ_HANDLED)
		DBG(5, "spurious?\n");

	return IRQ_HANDLED;
}

#else
#define generic_interrupt	NULL
#endif

/*
 * handle all the irqs defined by the HDRC core. for now we expect:  other
 * irq sources (phy, dma, etc) will be handled first, musb->int_* values
 * will be assigned, and the irq will already have been acked.
 *
 * called in irq context with spinlock held, irqs blocked
 */
irqreturn_t musb_interrupt(struct musb *musb)
{
	irqreturn_t	retval = IRQ_NONE;
	u8		devctl, power, int_usb;
	int		ep_num;
	u32		reg;

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	power = musb_readb(musb->mregs, MUSB_POWER);

	DBG(4, "** IRQ %s usb%04x tx%04x rx%04x\n",
		(devctl & MUSB_DEVCTL_HM) ? "host" : "peripheral",
		musb->int_usb, musb->int_tx, musb->int_rx);

#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	if (is_otg_enabled(musb)|| is_peripheral_enabled(musb))
		if (!musb->gadget_driver) {
			DBG(5, "No gadget driver loaded\n");
			musb->int_usb = 0;
			musb->int_tx = 0;
			musb->int_rx = 0;
			return IRQ_HANDLED;
		}
#endif

	/* the core can interrupt us for multiple reasons; docs have
	 * a generic interrupt flowchart to follow
	 */
	int_usb = musb->int_usb;
	musb->int_usb = 0;
	int_usb &= ~MUSB_INTR_SOF;
	if (int_usb)
		retval |= musb_stage0_irq(musb, int_usb, devctl, power);

	/* "stage 1" is handling endpoint irqs */

	/* handle endpoint 0 first */
	if (musb->int_tx & 1) {
		musb->int_tx &= ~1;
		if (devctl & MUSB_DEVCTL_HM)
			retval |= musb_h_ep0_irq(musb);
		else
			retval |= musb_g_ep0_irq(musb);
	}

	/* TX on endpoints 1-15 */
	reg = musb->int_tx >> 1;
	musb->int_tx = 0;
	ep_num = 1;
	while (reg) {
		if (reg & 1) {
			/* musb_ep_select(musb->mregs, ep_num); */
			/* REVISIT just retval |= ep->tx_irq(...) */
			retval = IRQ_HANDLED;
			if (devctl & MUSB_DEVCTL_HM) {
				if (is_host_capable())
					musb_host_tx(musb, ep_num);
			} else {
				if (is_peripheral_capable())
					musb_g_tx(musb, ep_num);
			}
		}
		reg >>= 1;
		ep_num++;
	}

	/* RX on endpoints 1-15 */
	reg = musb->int_rx >> 1;
	musb->int_rx = 0;
	ep_num = 1;
	while (reg) {
		if (reg & 1) {
			/* musb_ep_select(musb->mregs, ep_num); */
			/* REVISIT just retval = ep->rx_irq(...) */
			retval = IRQ_HANDLED;
			if (devctl & MUSB_DEVCTL_HM) {
				if (is_host_capable())
					musb_host_rx(musb, ep_num);
			} else {
				if (is_peripheral_capable())
					musb_g_rx(musb, ep_num, false);
			}
		}

		reg >>= 1;
		ep_num++;
	}

	return retval;
}


#ifndef CONFIG_MUSB_PIO_ONLY
void musb_dma_completion(struct musb *musb, u8 epnum, u8 transmit)
{
	u8	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	/* called with controller lock already held */

	if (!epnum) {
#ifndef CONFIG_USB_TUSB_OMAP_DMA
		if (!cppi_ti_dma()) {
			/* endpoint 0 */
			if (devctl & MUSB_DEVCTL_HM)
				musb_h_ep0_irq(musb);
			else
				musb_g_ep0_irq(musb);
		}
#endif
	} else {
		/* endpoints 1..15 */
		if (transmit) {
			if (devctl & MUSB_DEVCTL_HM) {
				if (is_host_capable())
					musb_host_tx(musb, epnum);
			} else {
				if (is_peripheral_capable())
					musb_g_tx(musb, epnum);
			}
		} else {
			/* receive */
			if (devctl & MUSB_DEVCTL_HM) {
				if (is_host_capable())
					musb_host_rx(musb, epnum);
			} else {
				if (is_peripheral_capable())
					musb_g_rx(musb, epnum, true);
			}
		}
	}
}
#endif

/*-------------------------------------------------------------------------*/

#ifdef CONFIG_SYSFS

static ssize_t
musb_charger_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);

	return sprintf(buf, "%d\n", (musb->is_charger ?
			musb->is_charger : musb_charger_detect(musb)));
}
static DEVICE_ATTR(charger, 0444, musb_charger_show, NULL);

static ssize_t
musb_amp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);

	return sprintf(buf, "%d\n", musb->power_draw);
}
static DEVICE_ATTR(mA, 0444, musb_amp_show, NULL);

static ssize_t
musb_hostdevice_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);

	return sprintf(buf, "%s\n", musb->hostdevice);
}
static DEVICE_ATTR(hostdevice, 0444, musb_hostdevice_show, NULL);

static ssize_t
musb_hostdevice2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);

	return sprintf(buf, "%s\n", musb->hostdevice2);
}
static DEVICE_ATTR(hostdevice2, 0444, musb_hostdevice2_show, NULL);

static ssize_t
musb_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);
	int ret = -EINVAL;

	mutex_lock(&musb->mutex);
	ret = sprintf(buf, "%s\n", otg_state_string(musb));
	mutex_unlock(&musb->mutex);

	return ret;
}

static ssize_t
musb_connect_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb	*musb = dev_to_musb(dev);
	unsigned long	flags;
	int		ret = -EINVAL;

	spin_lock_irqsave(&musb->lock, flags);
	ret = sprintf(buf, "%d\n", musb->softconnect);
	spin_unlock_irqrestore(&musb->lock, flags);

	return ret;
}

static ssize_t
musb_connect_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t n)
{
	struct musb	*musb = dev_to_musb(dev);
	unsigned long	flags;
	unsigned	val;
	int		status;
	u8		power;

	status = sscanf(buf, "%u", &val);
	if (status < 1) {
		printk(KERN_ERR "invalid parameter, %d\n", status);
		return -EINVAL;
	}

	spin_lock_irqsave(&musb->lock, flags);

	power = musb_readb(musb->mregs, MUSB_POWER);

	if (val)
		power |= MUSB_POWER_SOFTCONN;
	else
		power &= ~MUSB_POWER_SOFTCONN;

	musb->softconnect = !!val;
	musb_writeb(musb->mregs, MUSB_POWER, power);

	spin_unlock_irqrestore(&musb->lock, flags);

	return n;
}
static DEVICE_ATTR(connect, 0644, musb_connect_show, musb_connect_store);

static ssize_t
musb_mode_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t n)
{
	struct musb	*musb = dev_to_musb(dev);
	int		status;

	mutex_lock(&musb->mutex);
        if (sysfs_streq(buf, "hostl"))
                status = musb_platform_set_mode(musb, MUSB_HOST, 0);
        else if (sysfs_streq(buf, "hostf"))
                status = musb_platform_set_mode(musb, MUSB_HOST, 1);
        else if (sysfs_streq(buf, "hosth"))
                status = musb_platform_set_mode(musb, MUSB_HOST, 2);
	else if (sysfs_streq(buf, "peripheral"))
		status = musb_platform_set_mode(musb, MUSB_PERIPHERAL, 0);
	else if (sysfs_streq(buf, "otg"))
		status = musb_platform_set_mode(musb, MUSB_OTG, 0);
	else
		status = -EINVAL;
	mutex_unlock(&musb->mutex);

	musb->hostdevice = "none";
	musb->hostdevice2 = "none";
	sysfs_notify(&musb->controller->kobj, NULL, "hostdevice");
	sysfs_notify(&musb->controller->kobj, NULL, "hostdevice2");
	sysfs_notify(&musb->controller->kobj, NULL, "mode");
	schedule_work(&musb->irq_work);

	return (status == 0) ? n : status;
}
static DEVICE_ATTR(mode, 0644, musb_mode_show, musb_mode_store);

static ssize_t
musb_vbus_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t n)
{
	struct musb	*musb = dev_to_musb(dev);
	unsigned long	flags;
	unsigned long	val;

	if (sscanf(buf, "%lu", &val) < 1) {
		printk(KERN_ERR "Invalid VBUS timeout ms value\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&musb->lock, flags);
	musb->a_wait_bcon = val;
	if (musb->xceiv->state == OTG_STATE_A_WAIT_BCON)
		musb->is_active = 0;
	musb_platform_try_idle(musb, jiffies + msecs_to_jiffies(val));
	spin_unlock_irqrestore(&musb->lock, flags);

	return n;
}

static ssize_t
musb_vbus_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb	*musb = dev_to_musb(dev);
	unsigned long	flags;
	unsigned long	val;
	int		vbus;

	spin_lock_irqsave(&musb->lock, flags);
	val = musb->a_wait_bcon;
	vbus = musb_platform_get_vbus_status(musb);
	spin_unlock_irqrestore(&musb->lock, flags);

	return sprintf(buf, "Vbus %s, timeout %lu\n",
			vbus ? "on" : "off", val);
}
static DEVICE_ATTR(vbus, 0644, musb_vbus_show, musb_vbus_store);

#ifdef CONFIG_USB_GADGET_MUSB_HDRC

static ssize_t
musb_suspend_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);

	return sprintf(buf, "%d\n", musb->is_suspended);
}
static DEVICE_ATTR(suspend, 0444, musb_suspend_show, NULL);

/* Gadget drivers can't know that a host is connected so they might want
 * to start SRP, but users can.  This allows userspace to trigger SRP.
 */
static ssize_t
musb_srp_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t n)
{
	struct musb	*musb = dev_to_musb(dev);
	unsigned short	srp;

	if (sscanf(buf, "%hu", &srp) != 1
			|| (srp != 1)) {
		printk(KERN_ERR "SRP: Value must be 1\n");
		return -EINVAL;
	}

	if (srp == 1)
		musb_g_wakeup(musb);

	return n;
}
static DEVICE_ATTR(srp, 0644, NULL, musb_srp_store);

#endif /* CONFIG_USB_GADGET_MUSB_HDRC */

#endif	/* sysfs */

/* Only used to provide driver mode change events */
static void musb_irq_work(struct work_struct *data)
{
	struct musb *musb = container_of(data, struct musb, irq_work);
	static int old_state, old_ma, old_suspend;

	if (musb->xceiv->state != old_state) {
		old_state = musb->xceiv->state;
		musb->hostdevice = "none";
		musb->hostdevice2 = "none";
		sysfs_notify(&musb->controller->kobj, NULL, "hostdevice");
		sysfs_notify(&musb->controller->kobj, NULL, "hostdevice2");
		sysfs_notify(&musb->controller->kobj, NULL, "mode");
	}
	if (musb->power_draw != old_ma) {
		old_ma = musb->power_draw;
		sysfs_notify(&musb->controller->kobj, NULL, "mA");
	}
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	if (old_suspend != musb->is_suspended) {
		old_suspend = musb->is_suspended;
		sysfs_notify(&musb->controller->kobj, NULL, "suspend");
	}
#endif
}

/* --------------------------------------------------------------------------
 * Init support
 */

static struct musb *__init
allocate_instance(struct device *dev,
		struct musb_hdrc_config *config, void __iomem *mbase)
{
	struct musb		*musb;
	struct musb_hw_ep	*ep;
	int			epnum;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	struct usb_hcd	*hcd;

	hcd = usb_create_hcd(&musb_hc_driver, dev, dev_name(dev));
	if (!hcd)
		return NULL;
	/* usbcore sets dev->driver_data to hcd, and sometimes uses that... */

	musb = hcd_to_musb(hcd);

	hcd->uses_new_polling = 1;

	musb->vbuserr_retry = VBUSERR_RETRY_COUNT;
#else
	musb = kzalloc(sizeof *musb, GFP_KERNEL);
	if (!musb)
		return NULL;
	dev_set_drvdata(dev, musb);

#endif

	musb->mregs = mbase;
	musb->ctrl_base = mbase;
	musb->nIrq = -ENODEV;
	musb->config = config;
	BUG_ON(musb->config->num_eps > MUSB_C_NUM_EPS);
	for (epnum = 0, ep = musb->endpoints;
			epnum < musb->config->num_eps;
			epnum++, ep++) {
		ep->musb = musb;
		ep->epnum = epnum;
	}

	musb->controller = dev;
	return musb;
}

static void musb_free(struct musb *musb)
{
	/* this has multiple entry modes. it handles fault cleanup after
	 * probe(), where things may be partially set up, as well as rmmod
	 * cleanup after everything's been de-activated.
	 */

#ifdef CONFIG_SYSFS
	device_remove_file(musb->controller, &dev_attr_mA);
	device_remove_file(musb->controller, &dev_attr_connect);
	device_remove_file(musb->controller, &dev_attr_charger);
	device_remove_file(musb->controller, &dev_attr_hostdevice);
	device_remove_file(musb->controller, &dev_attr_hostdevice2);
	device_remove_file(musb->controller, &dev_attr_mode);
	device_remove_file(musb->controller, &dev_attr_vbus);
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	device_remove_file(musb->controller, &dev_attr_suspend);
	device_remove_file(musb->controller, &dev_attr_srp);
#endif
#endif

#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	musb_gadget_cleanup(musb);
#endif

	if (musb->nIrq >= 0) {
		if (musb->irq_wake)
			disable_irq_wake(musb->nIrq);
		free_irq(musb->nIrq, musb);
	}
	if (is_dma_capable() && musb->dma_controller) {
		struct dma_controller	*c = musb->dma_controller;

		(void) c->stop(c);
		dma_controller_destroy(c);
	}

	musb_writeb(musb->mregs, MUSB_DEVCTL, 0);
	musb_platform_exit(musb);
	musb_writeb(musb->mregs, MUSB_DEVCTL, 0);

	if (musb->clock) {
		clk_disable(musb->clock);
		clk_put(musb->clock);
	}

#ifdef CONFIG_USB_MUSB_OTG
	if (musb->xceiv)
		put_device(musb->xceiv->dev);
#endif

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	usb_put_hcd(musb_to_hcd(musb));
#else
	kfree(musb);
#endif
	the_musb = NULL;
}

/*
 * Perform generic per-controller initialization.
 *
 * @pDevice: the controller (already clocked, etc)
 * @nIrq: irq
 * @mregs: virtual address of controller registers,
 *	not yet corrected for platform-specific offsets
 */
static int __init
musb_init_controller(struct device *dev, int nIrq, void __iomem *ctrl)
{
	int			status;
	struct musb		*musb;
	struct musb_hdrc_platform_data *plat = dev->platform_data;

	/* The driver might handle more features than the board; OK.
	 * Fail when the board needs a feature that's not enabled.
	 */
	if (!plat) {
		dev_dbg(dev, "no platform_data?\n");
		return -ENODEV;
	}
	switch (plat->mode) {
	case MUSB_HOST:
#ifdef CONFIG_USB_MUSB_HDRC_HCD
		break;
#else
		goto bad_config;
#endif
	case MUSB_PERIPHERAL:
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
		break;
#else
		goto bad_config;
#endif
	case MUSB_OTG:
#ifdef CONFIG_USB_MUSB_OTG
		break;
#else
bad_config:
#endif
	default:
		dev_err(dev, "incompatible Kconfig role setting\n");
		return -EINVAL;
	}

	/* allocate */
	musb = allocate_instance(dev, plat->config, ctrl);
	if (!musb)
		return -ENOMEM;

	the_musb = musb;

	spin_lock_init(&musb->lock);
	mutex_init(&musb->mutex);
	musb->board = plat->board;
	musb->board_mode = plat->mode;
	musb->board_set_power = plat->set_power;
	musb->set_clock = plat->set_clock;
	musb->min_power = plat->min_power;
	musb->use_dma = use_dma;
	musb->hostdevice = "none";
	musb->hostdevice2 = "none";

	/* Clock usage is chip-specific ... functional clock (DaVinci,
	 * OMAP2430), or PHY ref (some TUSB6010 boards).  All this core
	 * code does is make sure a clock handle is available; platform
	 * code manages it during start/stop and suspend/resume.
	 */
	if (plat->clock) {
		musb->clock = clk_get(dev, plat->clock);
		if (IS_ERR(musb->clock)) {
			status = PTR_ERR(musb->clock);
			musb->clock = NULL;
			goto fail;
		}
	}

	/* assume vbus is off */

	/* platform adjusts musb->mregs and musb->isr if needed,
	 * and activates clocks
	 */
	musb->isr = generic_interrupt;
	status = musb_platform_init(musb);

	if (status < 0)
		goto fail;
	if (!musb->isr) {
		status = -ENODEV;
		goto fail2;
	}

#ifndef CONFIG_MUSB_PIO_ONLY
	if (use_dma && dev->dma_mask) {
		struct dma_controller	*c;

		c = dma_controller_create(musb, musb->mregs);
		musb->dma_controller = c;
		if (c)
			(void) c->start(c);
	}
#endif
	/* ideally this would be abstracted in platform setup */
	if (!musb->use_dma || !musb->dma_controller)
		dev->dma_mask = NULL;

	/* be sure interrupts are disabled before connecting ISR */
	musb_platform_disable(musb);
	musb_generic_disable(musb);

	/* setup musb parts of the core (especially endpoints) */
	status = musb_core_init(plat->config->multipoint
			? MUSB_CONTROLLER_MHDRC
			: MUSB_CONTROLLER_HDRC, musb);
	if (status < 0)
		goto fail2;

	/* Init IRQ workqueue before request_irq */
	INIT_WORK(&musb->irq_work, musb_irq_work);

	/* attach to the IRQ */
	if (request_irq(nIrq, musb->isr, 0, dev_name(dev), musb)) {
		dev_err(dev, "request_irq %d failed!\n", nIrq);
		status = -ENODEV;
		goto fail2;
	}
	musb->nIrq = nIrq;
/* FIXME this handles wakeup irqs wrong */
	if (enable_irq_wake(nIrq) == 0) {
		musb->irq_wake = 1;
		device_init_wakeup(dev, 1);
	} else {
		musb->irq_wake = 0;
	}

	pr_info("%s: USB %s mode controller at %p using %s, IRQ %d\n",
			musb_driver_name,
			({char *s;
			switch (musb->board_mode) {
			case MUSB_HOST:		s = "Host"; break;
			case MUSB_PERIPHERAL:	s = "Peripheral"; break;
			default:		s = "OTG"; break;
			}; s; }),
			ctrl,
			(is_dma_capable() && musb->dma_controller)
				? "DMA" : "PIO",
			musb->nIrq);

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* host side needs more setup, except for no-host modes */
	if (musb->board_mode != MUSB_PERIPHERAL) {
		struct usb_hcd	*hcd = musb_to_hcd(musb);

		if (musb->board_mode == MUSB_OTG)
			hcd->self.otg_port = 1;
		musb->xceiv->host = &hcd->self;
		hcd->power_budget = 2 * (plat->power ? : 250);
	}
#endif				/* CONFIG_USB_MUSB_HDRC_HCD */

	/* For the host-only role, we can activate right away.
	 * (We expect the ID pin to be forcibly grounded!!)
	 * Otherwise, wait till the gadget driver hooks up.
	 */
	if (!is_otg_enabled(musb) && is_host_enabled(musb)) {
		MUSB_HST_MODE(musb);
		musb->xceiv->default_a = 1;
		musb->xceiv->state = OTG_STATE_A_IDLE;

		status = usb_add_hcd(musb_to_hcd(musb), -1, 0);
		if (status)
			goto fail;

		DBG(1, "%s mode, status %d, devctl %02x %c\n",
			"HOST", status,
			musb_readb(musb->mregs, MUSB_DEVCTL),
			(musb_readb(musb->mregs, MUSB_DEVCTL)
					& MUSB_DEVCTL_BDEVICE
				? 'B' : 'A'));

	} else /* peripheral is enabled */ {
		MUSB_DEV_MODE(musb);
		musb->xceiv->default_a = 0;
		musb->xceiv->state = OTG_STATE_B_IDLE;

		status = musb_gadget_setup(musb);
		if (status)
			goto fail;

		DBG(1, "%s mode, status %d, dev%02x\n",
			is_otg_enabled(musb) ? "OTG" : "PERIPHERAL",
			status,
			musb_readb(musb->mregs, MUSB_DEVCTL));

	}

	if (!(musb_debug_create("driver/musb_hdrc", musb)))
		DBG(1, "could not create procfs entry\n");

#ifdef CONFIG_SYSFS
	status = device_create_file(dev, &dev_attr_mA);
	status = device_create_file(dev, &dev_attr_connect);
	status = device_create_file(dev, &dev_attr_charger);
	status = device_create_file(dev, &dev_attr_hostdevice);
	status = device_create_file(dev, &dev_attr_hostdevice2);
	status = device_create_file(dev, &dev_attr_mode);
	status = device_create_file(dev, &dev_attr_vbus);
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	status = device_create_file(dev, &dev_attr_suspend);
	status = device_create_file(dev, &dev_attr_srp);
#endif /* CONFIG_USB_GADGET_MUSB_HDRC */
	status = 0;
#endif
	if (status)
		goto fail2;

	/* Resets the controller. Has to be done. Without this, most likely
	 * the state machine inside the transceiver doesn't get fixed properly
	 */
	musb_save_ctx_and_suspend(&musb->g, 0);
	musb_restore_ctx_and_resume(&musb->g);

	return 0;

fail2:
#ifdef CONFIG_SYSFS
	device_remove_file(dev, &dev_attr_mA);
	device_remove_file(dev, &dev_attr_connect);
	device_remove_file(dev, &dev_attr_charger);
	device_remove_file(dev, &dev_attr_hostdevice);
	device_remove_file(dev, &dev_attr_hostdevice2);
	device_remove_file(musb->controller, &dev_attr_mode);
	device_remove_file(musb->controller, &dev_attr_vbus);
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	device_remove_file(musb->controller, &dev_attr_suspend);
	device_remove_file(musb->controller, &dev_attr_srp);
#endif
#endif
	musb_platform_exit(musb);
fail:
	dev_err(musb->controller,
		"musb_init_controller failed with status %d\n", status);

	if (musb->clock) {
		clk_put(musb->clock);
		musb->clock = NULL;
	}
	device_init_wakeup(dev, 0);
	musb_free(musb);

	return status;

}

/*-------------------------------------------------------------------------*/

/* all implementations (PCI bridge to FPGA, VLYNQ, etc) should just
 * bridge to a platform device; this driver then suffices.
 */

#ifndef CONFIG_MUSB_PIO_ONLY
static u64	*orig_dma_mask;
#endif

static int __init musb_probe(struct platform_device *pdev)
{
	struct device	*dev = &pdev->dev;
	int		irq = platform_get_irq(pdev, 0);
	struct resource	*iomem;
	void __iomem	*base;

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!iomem || irq == 0)
		return -ENODEV;

	base = ioremap(iomem->start, iomem->end - iomem->start + 1);
	if (!base) {
		dev_err(dev, "ioremap failed\n");
		return -ENOMEM;
	}

#ifndef CONFIG_MUSB_PIO_ONLY
	/* clobbered by use_dma=n */
	orig_dma_mask = dev->dma_mask;
#endif
	/* Store initial mask for USB interrupts */
	ctx.intrusbe = 0xf7;

	return musb_init_controller(dev, irq, base);
}

static int __devexit musb_remove(struct platform_device *pdev)
{
	struct musb	*musb = dev_to_musb(&pdev->dev);
	void __iomem	*ctrl_base = musb->ctrl_base;

	/* this gets called on rmmod.
	 *  - Host mode: host may still be active
	 *  - Peripheral mode: peripheral is deactivated (or never-activated)
	 *  - OTG mode: both roles are deactivated (or never-activated)
	 */
	musb_shutdown(pdev);
	musb_debug_delete("driver/musb_hdrc", musb);
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (musb->board_mode == MUSB_HOST)
		usb_remove_hcd(musb_to_hcd(musb));
#endif
	musb_free(musb);
	iounmap(ctrl_base);
	device_init_wakeup(&pdev->dev, 0);
#ifndef CONFIG_MUSB_PIO_ONLY
	pdev->dev.dma_mask = orig_dma_mask;
#endif
	return 0;
}

#ifdef	CONFIG_PM

void musb_save_ctx(struct musb *musb)
{
	ctx.power = musb_readb(musb->mregs, MUSB_POWER);
	ctx.intrtxe = musb_readw(musb->mregs, MUSB_INTRTXE);
	ctx.intrrxe = musb_readw(musb->mregs, MUSB_INTRRXE);
	ctx.intrusbe = musb_readb(musb->mregs, MUSB_INTRUSBE);
	ctx.devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
}

void musb_restore_ctx(struct musb *musb)
{
	int i;
	musb_writeb(musb->mregs, MUSB_POWER, ctx.power);
	musb_writew(musb->mregs, MUSB_INTRTX, 0x00);
	musb_writew(musb->mregs, MUSB_INTRTXE, ctx.intrtxe);
	musb_writew(musb->mregs, MUSB_INTRRX, 0x00);
	musb_writew(musb->mregs, MUSB_INTRRXE, ctx.intrrxe);
	musb_writeb(musb->mregs, MUSB_INTRUSB, 0x00);
	musb_writeb(musb->mregs, MUSB_INTRUSBE, ctx.intrusbe);
	musb_writeb(musb->mregs, MUSB_DEVCTL, ctx.devctl);

	/* iterate over every endpoint, select it and restore its context */
	for (i = 0; i < musb->config->num_eps; i++) {
		musb_writeb(musb->mregs, MUSB_INDEX, i);
		musb_writeb(musb->mregs, MUSB_RXFIFOSZ, ctx.rxfifosz[i]);
		musb_writeb(musb->mregs, MUSB_TXFIFOSZ, ctx.txfifosz[i]);
		musb_writew(musb->mregs, MUSB_TXFIFOADD, ctx.txfifoadd[i]);
		musb_writew(musb->mregs, MUSB_RXFIFOADD, ctx.rxfifoadd[i]);
	};
}

static int musb_suspend(struct platform_device *pdev, pm_message_t message)
{
	unsigned long	flags;
	struct musb	*musb = dev_to_musb(&pdev->dev);


	spin_lock_irqsave(&musb->lock, flags);

	if (is_peripheral_active(musb)) {
		/* FIXME force disconnect unless we know USB will wake
		 * the system up quickly enough to respond ...
		 */
	} else if (is_host_active(musb)) {
		/* we know all the children are suspended; sometimes
		 * they will even be wakeup-enabled.
		 */
	}

	/* save context, actually don't, the hardware register context 'now'
	 * is in such a state that restoring it later will prevent talking to
	 * a computer USB port (or charging).  However leaving the register
	 * context as stored previously will allow it to work.
	 *
	 * musb_save_ctx(musb);
	 */

	if (!musb->clock) {
		if (musb->set_clock)
			musb->set_clock(musb->clock, 0);
		else
			clk_disable(musb->clock);
	}

	spin_unlock_irqrestore(&musb->lock, flags);

	musb_hnp_stop(musb);
	musb_pullup(musb, 0);
	musb_stop(musb);
	return 0;
}

static int musb_resume(struct platform_device *pdev)
{
	unsigned long	flags;
	struct musb	*musb = dev_to_musb(&pdev->dev);


	spin_lock_irqsave(&musb->lock, flags);

	if (!musb->clock) {
		if (musb->set_clock)
			musb->set_clock(musb->clock, 1);
		else
			clk_enable(musb->clock);
	}

	/* restore context */
	musb_restore_ctx(musb);

	/* for static cmos like DaVinci, register values were preserved
	 * unless for some reason the whole soc powered down and we're
	 * not treating that as a whole-system restart (e.g. swsusp)
	 */
	spin_unlock_irqrestore(&musb->lock, flags);
	if(musb->xceiv && musb->xceiv->gadget)
		musb_start(musb);
	return 0;
}

#else
#define	musb_suspend	NULL
#define	musb_resume	NULL
#endif

static struct platform_driver musb_driver = {
	.driver = {
		.name		= (char *)musb_driver_name,
		.bus		= &platform_bus_type,
		.owner		= THIS_MODULE,
	},
	.remove		= __devexit_p(musb_remove),
	.shutdown	= musb_shutdown,
	.suspend	= musb_suspend,
	.resume		= musb_resume,
};

/*-------------------------------------------------------------------------*/

static int __init musb_init(void)
{
	int result;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (usb_disabled())
		return 0;
#endif

	pr_info("%s: version " MUSB_VERSION ", "
#ifdef CONFIG_MUSB_PIO_ONLY
		"pio"
#elif defined(CONFIG_USB_TI_CPPI_DMA)
		"cppi-dma"
#elif defined(CONFIG_USB_INVENTRA_DMA)
		"musb-dma"
#elif defined(CONFIG_USB_TUSB_OMAP_DMA)
		"tusb-omap-dma"
#else
		"?dma?"
#endif
		", "
#ifdef CONFIG_USB_MUSB_OTG
		"otg (peripheral+host)"
#elif defined(CONFIG_USB_GADGET_MUSB_HDRC)
		"peripheral"
#elif defined(CONFIG_USB_MUSB_HDRC_HCD)
		"host"
#endif
		", debug=%d\n",
		musb_driver_name, musb_debug);
	result = platform_driver_probe(&musb_driver, musb_probe);
	if (!result) {
		musb_emergency_stop_ptr=musb_emergency_stop;
		rx51_detect_wallcharger_ptr=rx51_detect_wallcharger;
	}
	return result;
}

/* make us init after usbcore and before usb
 * gadget and host-side drivers start to register
 */
subsys_initcall(musb_init);

static void __exit musb_cleanup(void)
{
	if(the_musb) {
		musb_hnp_stop(the_musb);
		musb_pullup(the_musb, 0);
		musb_stop(the_musb);
	}
	platform_driver_unregister(&musb_driver);
	musb_emergency_stop_ptr=NULL;
	rx51_detect_wallcharger_ptr=NULL;
}
module_exit(musb_cleanup);
