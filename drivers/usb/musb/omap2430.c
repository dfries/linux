/*
 * Copyright (C) 2005-2007 by Texas Instruments
 * Some code has been taken from tusb6010.c
 * Copyrights for that are attributable to:
 * Copyright (C) 2006 Nokia Corporation
 * Jarkko Nikula <jarkko.nikula@nokia.com>
 * Tony Lindgren <tony@atomide.com>
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * The Inventra Controller Driver for Linux is free software; you
 * can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * The Inventra Controller Driver for Linux is distributed in
 * the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The Inventra Controller Driver for Linux ; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <asm/mach-types.h>
#include <mach/hardware.h>
#include <mach/mux.h>

#include <mach/board-rx51.h>

#include <linux/i2c/twl4030.h>

#include "musb_core.h"
#include "omap2430.h"

#ifdef CONFIG_ARCH_OMAP3430
#define	get_cpu_rev()	2
#endif

#define MUSB_TIMEOUT_A_WAIT_BCON	1100

static struct timer_list musb_idle_timer;

static void musb_vbus_work(struct work_struct *data)
{
	struct musb *musb = container_of(data, struct musb, vbus_work);
	u8 devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	/* clear/set requirements for musb to work with DPS on omap3 */
	if (musb->board && musb->board->set_pm_limits && !musb->is_charger)
		musb->board->set_pm_limits(musb->controller,
					(devctl & MUSB_DEVCTL_VBUS));
}

static void musb_do_idle(unsigned long _musb)
{
	struct musb	*musb = (void *)_musb;
	unsigned long	flags;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	u8	power;
#endif
	u8	devctl;

	spin_lock_irqsave(&musb->lock, flags);

	DBG(3, "%s\n", otg_state_string(musb));

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	switch (musb->xceiv->state) {
	case OTG_STATE_A_WAIT_BCON:
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		break;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_SUSPEND:
		/* finish RESUME signaling? */
		if (musb->port1_status & MUSB_PORT_STAT_RESUME) {
			power = musb_readb(musb->mregs, MUSB_POWER);
			power &= ~MUSB_POWER_RESUME;
			DBG(1, "root port resume stopped, power %02x\n", power);
			musb_writeb(musb->mregs, MUSB_POWER, power);
			musb->is_active = 1;
			musb->port1_status &= ~(USB_PORT_STAT_SUSPEND
						| MUSB_PORT_STAT_RESUME);
			musb->port1_status |= USB_PORT_STAT_C_SUSPEND << 16;
			usb_hcd_poll_rh_status(musb_to_hcd(musb));
			/* NOTE: it might really be A_WAIT_BCON ... */
			musb->xceiv->state = OTG_STATE_A_HOST;
		}
		break;
#endif
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_HOST:
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl &  MUSB_DEVCTL_BDEVICE)
			musb->xceiv->state = OTG_STATE_B_IDLE;
		else
			musb->xceiv->state = OTG_STATE_A_WAIT_BCON;
#endif
	default:
		break;
	}
	spin_unlock_irqrestore(&musb->lock, flags);
}


void musb_platform_try_idle(struct musb *musb, unsigned long timeout)
{
	unsigned long		default_timeout = jiffies + msecs_to_jiffies(3);
	static unsigned long	last_timer;

	if (timeout == 0)
		timeout = default_timeout;

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active || ((musb->a_wait_bcon == 0)
			&& (musb->xceiv->state == OTG_STATE_A_WAIT_BCON))) {
		DBG(4, "%s active, deleting timer\n", otg_state_string(musb));
		del_timer(&musb_idle_timer);
		last_timer = jiffies;
		return;
	}

	if (time_after(last_timer, timeout)) {
		if (!timer_pending(&musb_idle_timer))
			last_timer = timeout;
		else {
			DBG(4, "Longer idle timer already pending, ignoring\n");
			return;
		}
	}
	last_timer = timeout;

	DBG(4, "%s inactive, for idle timer for %lu ms\n",
		otg_state_string(musb),
		(unsigned long)jiffies_to_msecs(timeout - jiffies));
	mod_timer(&musb_idle_timer, timeout);
}

#ifdef CONFIG_PM
extern void (*musb_save_ctx_and_suspend_ptr)(struct usb_gadget *gadget,
	int overwrite);
extern void (*musb_restore_ctx_and_resume_ptr)(struct usb_gadget *gadget);
#endif

void musb_platform_enable(struct musb *musb)
{
	twl4030_upd_usb_suspended(0);
}
void musb_platform_disable(struct musb *musb)
{
	twl4030_upd_usb_suspended(musb->is_suspended);
}
static void omap_vbus_power(struct musb *musb, int is_on, int sleeping)
{
}

static void omap_set_vbus(struct musb *musb, int is_on)
{
	u8		devctl;
	/* HDRC controls CPEN, but beware current surges during device
	 * connect.  They can trigger transient overcurrent conditions
	 * that must be ignored.
	 */

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	if (is_on) {
		musb->is_active = 1;
		musb->xceiv->default_a = 1;
		musb->xceiv->state = OTG_STATE_A_WAIT_VRISE;
		devctl |= MUSB_DEVCTL_SESSION;

		MUSB_HST_MODE(musb);
	} else {
		musb->is_active = 0;

		/* NOTE:  we're skipping A_WAIT_VFALL -> A_IDLE and
		 * jumping right to B_IDLE...
		 */

		musb->xceiv->default_a = 0;
		musb->xceiv->state = OTG_STATE_B_IDLE;
		devctl &= ~MUSB_DEVCTL_SESSION;

		MUSB_DEV_MODE(musb);
	}
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

	DBG(1, "VBUS %s, devctl %02x "
		/* otg %3x conf %08x prcm %08x */ "\n",
		otg_state_string(musb),
		musb_readb(musb->mregs, MUSB_DEVCTL));
}
static int omap_set_power(struct otg_transceiver *x, unsigned mA)
{
	return 0;
}

static int musb_platform_resume(struct musb *musb);
static int musb_platform_suspend(struct musb *musb);

int musb_platform_set_mode(struct musb *musb, u8 musb_mode, u8 hostspeed)
{
	struct usb_hcd	*hcd;
	struct usb_bus	*host;
	u8		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	switch (musb_mode) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case MUSB_HOST:
		hcd = musb_to_hcd(musb);
		host = hcd_to_bus(hcd);

		otg_set_host(musb->xceiv, host);
 
                if (machine_is_nokia_rx51()) {
                        u8 testmode;
                        rx51_enable_charger_detection(0);
 
                        musb_platform_resume(musb);
 
                        devctl |= MUSB_DEVCTL_SESSION;
                        musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
 
                        testmode = MUSB_TEST_FORCE_HOST;
                        if (hostspeed == 1)
                                testmode |= MUSB_TEST_FORCE_FS;
                        else if (hostspeed == 2)
                                testmode |= MUSB_TEST_FORCE_HS;
                        musb_writeb(musb->mregs, MUSB_TESTMODE, testmode);
                }
		break;
#endif
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	case MUSB_PERIPHERAL:
                if (machine_is_nokia_rx51()) {
                        musb_platform_resume(musb);
                        musb_set_vbus(musb, 0);
 
                        devctl &= ~MUSB_DEVCTL_SESSION;
                        musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
 
                        musb_writeb(musb->mregs, MUSB_TESTMODE, 0);
			musb_platform_suspend(musb);
                        rx51_enable_charger_detection(1);
                }
 
		otg_set_peripheral(musb->xceiv, &musb->g);
		break;
#endif
#ifdef CONFIG_USB_MUSB_OTG
	case MUSB_OTG:
		break;
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

int __init musb_platform_init(struct musb *musb)
{
	struct otg_transceiver *x = otg_get_transceiver();
	u32 l;

#if defined(CONFIG_ARCH_OMAP2430)
	omap_cfg_reg(AE5_2430_USB0HS_STP);
#endif

	musb->suspendm = true;
	musb->xceiv = x;
	musb_platform_resume(musb);

	if (!x)
		return -ENODEV;

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~ENABLEWAKEUP;	/* disable wakeup */
	l &= ~NOSTDBY;		/* remove possible nostdby */
	l |= SMARTSTDBY;	/* enable smart standby */
	l &= ~AUTOIDLE;		/* disable auto idle */
	l &= ~NOIDLE;		/* remove possible noidle */
	l |= SMARTIDLE;		/* enable smart idle */
	/*
	 * MUSB AUTOIDLE don't work in 3430.
	 * Workaround by Richard Woodruff/TI
	 */
	if (!cpu_is_omap3430())
		l |= AUTOIDLE;	/* enable auto idle */
	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_INTERFSEL);
	l |= ULPI_12PIN;
	omap_writel(l, OTG_INTERFSEL);

#ifdef CONFIG_PM
	musb_save_ctx_and_suspend_ptr = musb_save_ctx_and_suspend;
	musb_restore_ctx_and_resume_ptr = musb_restore_ctx_and_resume;
#endif

	pr_debug("HS USB OTG: revision 0x%x, sysconfig 0x%02x, "
			"sysstatus 0x%x, intrfsel 0x%x, simenable  0x%x\n",
			omap_readl(OTG_REVISION), omap_readl(OTG_SYSCONFIG),
			omap_readl(OTG_SYSSTATUS), omap_readl(OTG_INTERFSEL),
			omap_readl(OTG_SIMENABLE));

	omap_vbus_power(musb, musb->board_mode == MUSB_HOST, 1);

	if (is_host_enabled(musb))
		musb->board_set_vbus = omap_set_vbus;
	if (is_peripheral_enabled(musb))
		musb->xceiv->set_power = omap_set_power;
	musb->a_wait_bcon = MUSB_TIMEOUT_A_WAIT_BCON;

	setup_timer(&musb_idle_timer, musb_do_idle, (unsigned long) musb);
	INIT_WORK(&musb->vbus_work, musb_vbus_work);

	return 0;
}

int musb_platform_suspend(struct musb *musb)
{
	u32 l;

	if (!musb->clock)
		return 0;

	/* in any role */
	l = omap_readl(OTG_FORCESTDBY);
	l |= ENABLEFORCE;	/* enable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	l = omap_readl(OTG_SYSCONFIG);
	l |= ENABLEWAKEUP;	/* enable wakeup */
	omap_writel(l, OTG_SYSCONFIG);

	if (musb->xceiv->set_suspend)
		musb->xceiv->set_suspend(musb->xceiv, 1);

	if (musb->set_clock)
		musb->set_clock(musb->clock, 0);
	else
		clk_disable(musb->clock);

	return 0;
}

static int musb_platform_resume(struct musb *musb)
{
	u32 l;

	if (!musb || !musb->xceiv || !musb->clock)
		return -1;

	if (musb->xceiv->set_suspend)
		musb->xceiv->set_suspend(musb->xceiv, 0);

	if (musb->set_clock)
		musb->set_clock(musb->clock, 1);
	else
		clk_enable(musb->clock);

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~ENABLEWAKEUP;	/* disable wakeup */
	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_FORCESTDBY);
	l &= ~ENABLEFORCE;	/* disable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	return 0;
}


int musb_platform_exit(struct musb *musb)
{
#ifdef CONFIG_PM
	musb_save_ctx_and_suspend_ptr = NULL;
	musb_restore_ctx_and_resume_ptr = NULL;
#endif

	omap_vbus_power(musb, 0 /*off*/, 1);

	musb_platform_suspend(musb);

	if (musb->clock)
		clk_put(musb->clock);
	musb->clock = 0;

	return 0;
}

#ifdef CONFIG_PM

void musb_save_ctx_and_suspend(struct usb_gadget *gadget, int overwrite)
{
	struct musb *musb = gadget_to_musb(gadget);
	u32 l;
	unsigned long	flags;
	unsigned long	tmo;

	spin_lock_irqsave(&musb->lock, flags);
	if (overwrite)
		/* Save register context */
		musb_save_ctx(musb);
	spin_unlock_irqrestore(&musb->lock, flags);

	DBG(3, "allow sleep\n");
	/* Do soft reset. This needs to be done with broken AUTOIDLE */
	tmo = jiffies + msecs_to_jiffies(300);
	omap_writel(SOFTRST, OTG_SYSCONFIG);
	while (!omap_readl(OTG_SYSSTATUS)) {
		if (time_after(jiffies, tmo)) {
			WARN(1, "musb failed to recover from reset!");
			break;
		}
	}

	l = omap_readl(OTG_FORCESTDBY);
	l |= ENABLEFORCE;	/* enable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	l = ENABLEWAKEUP;	/* enable wakeup */
	omap_writel(l, OTG_SYSCONFIG);
	/* Use AUTOIDLE here or the device may fail to hit sleep */
	l |= AUTOIDLE;
	omap_writel(l, OTG_SYSCONFIG);

	if (musb->board && musb->board->xceiv_power)
		musb->board->xceiv_power(0);
	/* Now it's safe to get rid of the buggy AUTOIDLE */
	l &= ~AUTOIDLE;
	omap_writel(l, OTG_SYSCONFIG);

	musb->is_charger = 0;

	if (machine_is_nokia_rx51() && rx51_with_charger_detection())
		rx51_set_wallcharger(0);

	/* clear constraints */
	if (musb->board && musb->board->set_pm_limits)
		musb->board->set_pm_limits(musb->controller, 0);
}

void musb_restore_ctx_and_resume(struct usb_gadget *gadget)
{
	struct musb *musb = gadget_to_musb(gadget);
	u32 l;
	u8 r;
	unsigned long	flags;

	DBG(3, "restoring register context\n");

	if (musb->board && musb->board->xceiv_power)
		musb->board->xceiv_power(1);

	spin_lock_irqsave(&musb->lock, flags);
	if (musb->set_clock)
		musb->set_clock(musb->clock, 1);
	else
		clk_enable(musb->clock);

	/* Recover OTG control */
	r = musb_ulpi_readb(musb->mregs, ISP1704_OTG_CTRL);
	r |= ISP1704_OTG_CTRL_IDPULLUP | ISP1704_OTG_CTRL_DP_PULLDOWN;
	musb_ulpi_writeb(musb->mregs, ISP1704_OTG_CTRL, r);

	/* Recover FUNC control */
	r = ISP1704_FUNC_CTRL_FULL_SPEED;
	r |= ISP1704_FUNC_CTRL_SUSPENDM | ISP1704_FUNC_CTRL_RESET;
	musb_ulpi_writeb(musb->mregs, ISP1704_FUNC_CTRL, r);

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~ENABLEWAKEUP;	/* disable wakeup */
	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_FORCESTDBY);
	l &= ~ENABLEFORCE;	/* disable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~ENABLEWAKEUP;	/* disable wakeup */
	l &= ~NOSTDBY;		/* remove possible nostdby */
	l |= SMARTSTDBY;	/* enable smart standby */
	l &= ~AUTOIDLE;		/* disable auto idle */
	l &= ~NOIDLE;		/* remove possible noidle */
	l |= SMARTIDLE;		/* enable smart idle */
	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_INTERFSEL);
	l |= ULPI_12PIN;
	omap_writel(l, OTG_INTERFSEL);

	/* Restore register context */
	musb_restore_ctx(musb);

	/* set constraints */
	schedule_work(&musb->vbus_work);
	spin_unlock_irqrestore(&musb->lock, flags);
}
#endif
