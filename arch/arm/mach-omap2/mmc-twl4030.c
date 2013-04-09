/*
 * linux/arch/arm/mach-omap2/mmc-twl4030.c
 *
 * Copyright (C) 2007-2008 Texas Instruments
 * Copyright (C) 2008 Nokia Corporation
 * Author: Texas Instruments
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/i2c/twl4030.h>

#include <mach/hardware.h>
#include <mach/control.h>
#include <mach/mmc.h>
#include <mach/board.h>

#include "mmc-twl4030.h"

#define MMCHS1				(L4_34XX_BASE + 0x9C000)
#define MMCHS2				(L4_34XX_BASE + 0xB4000)
#define MMCHS3				(L4_34XX_BASE + 0xAD000)
#define MAX_MMC				3
#define MMCHS_SYSCONFIG			0x0010
#define MMCHS_SYSCONFIG_SWRESET		(1 << 1)
#define MMCHS_SYSSTATUS			0x0014
#define MMCHS_SYSSTATUS_RESETDONE	(1 << 0)

static struct platform_device dummy_pdev = {
	.dev = {
		.bus = &platform_bus_type,
	},
};

/**
 * hsmmc_reset() - Full reset of each HS-MMC controller
 *
 * Ensure that each MMC controller is fully reset.  Controllers
 * left in an unknown state (by bootloaer) may prevent retention
 * or OFF-mode.  This is especially important in cases where the
 * MMC driver is not enabled, _or_ built as a module.
 *
 * In order for reset to work, interface, functional and debounce
 * clocks must be enabled.  The debounce clock comes from func_32k_clk
 * and is not under SW control, so we only enable i- and f-clocks.
 **/
static void __init hsmmc_reset(void)
{
	u32 i, base[MAX_MMC] = {MMCHS1, MMCHS2, MMCHS3};

	for (i = 0; i < MAX_MMC; i++) {
		u32 v;
		struct clk *iclk, *fclk;
		struct device *dev = &dummy_pdev.dev;

		dummy_pdev.id = i;
		iclk = clk_get(dev, "mmchs_ick");
		if (iclk && clk_enable(iclk))
			iclk = NULL;

		fclk = clk_get(dev, "mmchs_fck");
		if (fclk && clk_enable(fclk))
			fclk = NULL;

		if (!iclk || !fclk) {
			printk(KERN_WARNING
			       "%s: Unable to enable clocks for MMC%d, "
			       "cannot reset.\n",  __func__, i);
			break;
		}

		omap_writel(MMCHS_SYSCONFIG_SWRESET, base[i] + MMCHS_SYSCONFIG);
		v = omap_readl(base[i] + MMCHS_SYSSTATUS);
		while (!(omap_readl(base[i] + MMCHS_SYSSTATUS) &
			 MMCHS_SYSSTATUS_RESETDONE))
			cpu_relax();

		if (fclk) {
			clk_disable(fclk);
			clk_put(fclk);
		}
		if (iclk) {
			clk_disable(iclk);
			clk_put(iclk);
		}
	}
}

#if defined(CONFIG_TWL4030_CORE) && \
	(defined(CONFIG_MMC_OMAP_HS) || defined(CONFIG_MMC_OMAP_HS_MODULE))

#define LDO_CLR			0x00
#define VSEL_S2_CLR		0x40
#define VMMC_DEV_GRP_P1		0x20
#define DEDICATED_OFFSET	3
#define VMMC_DEV_GRP(c)		(c->twl_vmmc_dev_grp)

#define VAUX3_DEV_GRP		0x1F
#define VMMC1_DEV_GRP		0x27
#define VMMC2_DEV_GRP		0x2B
#define VSIM_DEV_GRP		0x37

#define VMMC1_315V		0x03
#define VMMC1_300V		0x02
#define VMMC1_285V		0x01
#define VMMC1_185V		0x00

#define VMMC2_315V		0x0c
#define VMMC2_300V		0x0b
#define VMMC2_285V		0x0a
#define VMMC2_280V		0x09
#define VMMC2_260V		0x08
#define VMMC2_185V		0x06

#define VAUX3_300V		0x04
#define VAUX3_280V		0x03
#define VAUX3_250V		0x02
#define VAUX3_180V		0x01
#define VAUX3_150V		0x00

#define VSIM_18V		0x03

static u16 control_pbias_offset;
static u16 control_devconf1_offset;

#define HSMMC_NAME_LEN	9

static struct twl_mmc_controller {
	struct omap_mmc_platform_data	*mmc;
	u8		twl_vmmc_dev_grp;
	bool		vsim_18v;
	char		name[HSMMC_NAME_LEN + 1];
} hsmmc[] = {
	{
		.twl_vmmc_dev_grp		= VMMC1_DEV_GRP,
	},
	{
		.twl_vmmc_dev_grp		= VMMC2_DEV_GRP,
	},
};

static int twl_mmc_card_detect(int irq)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(hsmmc); i++) {
		struct omap_mmc_platform_data *mmc;

		mmc = hsmmc[i].mmc;
		if (!mmc)
			continue;
		if (irq != mmc->slots[0].card_detect_irq)
			continue;

		/* NOTE: assumes card detect signal is active-low */
		return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
	}
	return -ENOSYS;
}

static int twl_mmc_get_ro(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes write protect signal is active-high */
	return gpio_get_value_cansleep(mmc->slots[0].gpio_wp);
}

static int twl_mmc_get_cover_state(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes card detect signal is active-low */
	return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
}

/*
 * MMC Slot Initialization.
 */
static int twl_mmc_late_init(struct device *dev)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;
	int ret = 0;
	int i;

	ret = gpio_request(mmc->slots[0].switch_pin, "mmc_cd");
	if (ret)
		goto done;
	ret = gpio_direction_input(mmc->slots[0].switch_pin);
	if (ret)
		goto err;

	for (i = 0; i < ARRAY_SIZE(hsmmc); i++) {
		if (hsmmc[i].name == mmc->slots[0].name) {
			hsmmc[i].mmc = mmc;
			break;
		}
	}

	return 0;

err:
	gpio_free(mmc->slots[0].switch_pin);
done:
	mmc->slots[0].card_detect_irq = 0;
	mmc->slots[0].card_detect = NULL;

	dev_err(dev, "err %d configuring card detect\n", ret);
	return ret;
}

static void twl_mmc_cleanup(struct device *dev)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	gpio_free(mmc->slots[0].switch_pin);
}

#ifdef CONFIG_PM

static int twl_mmc_suspend(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	disable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

static int twl_mmc_resume(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	enable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

#else
#define twl_mmc_suspend	NULL
#define twl_mmc_resume	NULL
#endif

/*
 * Sets the MMC voltage in twl4030
 */

#define MMC1_OCR	(MMC_VDD_165_195 \
		|MMC_VDD_28_29|MMC_VDD_29_30|MMC_VDD_30_31|MMC_VDD_31_32)
#define MMC2_OCR	(MMC_VDD_165_195 \
		|MMC_VDD_25_26|MMC_VDD_26_27|MMC_VDD_27_28 \
		|MMC_VDD_28_29|MMC_VDD_29_30|MMC_VDD_30_31|MMC_VDD_31_32)


#define VMMC1_ID 5
#define VMMC2_ID 6
#define VAUX3_ID 3
#define VSIM_ID 9


static int twl_mmc_i2c_wait(void)
{
	int ret, timeout = 100;
	u8 status;

	do {
		ret = twl4030_i2c_read_u8(TWL4030_MODULE_PM_MASTER,
					  &status, 0x14);
		if (ret)
			return ret;

		if (!(status & 1))
			return 0;

		msleep(10);

	} while (--timeout > 0);

	return -1;
}

static int twl_mmc_enable_regulator(u8 vmmc_dev_grp)
{
	int ret;
	u16 msg;
	u8 reg_id, pwb_state;

	switch (vmmc_dev_grp) {
	case VMMC1_DEV_GRP:
		reg_id = VMMC1_ID;
		break;
	case VMMC2_DEV_GRP:
		reg_id = VMMC2_ID;
		break;
	case VAUX3_DEV_GRP:
		reg_id = VAUX3_ID;
		break;
	case VSIM_DEV_GRP:
		reg_id = VSIM_ID;
		break;
	default:
		printk(KERN_ERR "twl_mmc_enable_regulator: unknown dev grp\n");
		return -1;
	}

	/* add regulator to dev grp P1 */
	ret = twl4030_i2c_write_u8(TWL4030_MODULE_PM_RECEIVER,
				   VMMC_DEV_GRP_P1, vmmc_dev_grp);
	if (ret)
		return ret;

	/* construct message to enable regulator on P1 */
	msg = (1 << 13) | (reg_id << 4) | 0xe;

	ret = twl4030_i2c_read_u8(TWL4030_MODULE_PM_MASTER,
				  &pwb_state, 0x14);
	if (ret)
		return ret;

	ret = twl4030_i2c_write_u8(TWL4030_MODULE_PM_MASTER,
				   pwb_state | (1 << 1), 0x14);
	if (ret)
		return ret;

	ret = twl_mmc_i2c_wait();
	if (ret)
		goto out;

	ret = twl4030_i2c_write_u8(TWL4030_MODULE_PM_MASTER,
				   msg >> 8, 0x15);
	if (ret)
		goto out;

	ret = twl4030_i2c_write_u8(TWL4030_MODULE_PM_MASTER,
				   msg & 0xff, 0x16);
	if (ret)
		goto out;

	ret = twl_mmc_i2c_wait();
	if (ret)
		goto out;

out:
	/* restore the previous state of TWL4030 */
	twl4030_i2c_write_u8(TWL4030_MODULE_PM_MASTER,
			     pwb_state, 0x14);

	if (ret)
		return ret;

	return 0;
}

static int twl_mmc_set_regulator(u8 vmmc_dev_grp, u8 vmmc)
{
	int ret;

	ret = twl_mmc_enable_regulator(vmmc_dev_grp);
	if (ret)
		return ret;

	ret = twl4030_i2c_write_u8(TWL4030_MODULE_PM_RECEIVER,
				   vmmc, vmmc_dev_grp + DEDICATED_OFFSET);

	return ret;
}

static int twl_mmc_shutdown_regulator(u8 vmmc_dev_grp)
{
	return twl4030_i2c_write_u8(TWL4030_MODULE_PM_RECEIVER,
				    LDO_CLR, vmmc_dev_grp);
}

static int twl_mmc_set_voltage(struct twl_mmc_controller *c, int vdd)
{
	int ret;
	u8 vmmc;

	if (c->twl_vmmc_dev_grp == VMMC1_DEV_GRP) {
		/* VMMC1:  max 220 mA.  And for 8-bit mode,
		 * VSIM:  max 50 mA
		 */
		switch (1 << vdd) {
		case MMC_VDD_165_195:
			vmmc = VMMC1_185V;
			/* and VSIM_180V */
			break;
		case MMC_VDD_28_29:
			vmmc = VMMC1_285V;
			/* and VSIM_280V */
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			vmmc = VMMC1_300V;
			/* and VSIM_300V */
			break;
		case MMC_VDD_31_32:
			vmmc = VMMC1_315V;
			/* error if VSIM needed */
			break;
		default:
			vmmc = 0;
			break;
		}
	} else if (c->twl_vmmc_dev_grp == VAUX3_DEV_GRP) {
		/* VAUX3:  max 200 mA */
		switch (1 << vdd) {
		case MMC_VDD_165_195:
			vmmc = VAUX3_180V;
			break;
		case MMC_VDD_25_26:
		case MMC_VDD_26_27:
			vmmc = VAUX3_250V;
			break;
		case MMC_VDD_27_28:
			vmmc = VAUX3_280V;
			break;
		case MMC_VDD_28_29:
			vmmc = VAUX3_280V;
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			vmmc = VAUX3_300V;
			break;
		case MMC_VDD_31_32:
			vmmc = VAUX3_300V;
			break;
		default:
			vmmc = 0;
			break;
		}
	} else if (c->twl_vmmc_dev_grp == VMMC2_DEV_GRP) {
		/* VMMC2:  max 100 mA */
		switch (1 << vdd) {
		case MMC_VDD_165_195:
			vmmc = VMMC2_185V;
			break;
		case MMC_VDD_25_26:
		case MMC_VDD_26_27:
			vmmc = VMMC2_260V;
			break;
		case MMC_VDD_27_28:
			vmmc = VMMC2_280V;
			break;
		case MMC_VDD_28_29:
			vmmc = VMMC2_285V;
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			vmmc = VMMC2_300V;
			break;
		case MMC_VDD_31_32:
			vmmc = VMMC2_315V;
			break;
		default:
			vmmc = 0;
			break;
		}
	} else {
		return 0;
	}

	if (vmmc) {
		ret = twl_mmc_set_regulator(VMMC_DEV_GRP(c), vmmc);
		if (ret)
			return ret;

		if (c->vsim_18v)
			ret = twl_mmc_set_regulator(VSIM_DEV_GRP, VSIM_18V);
	} else {
		ret = twl_mmc_shutdown_regulator(VMMC_DEV_GRP(c));
		if (ret)
			return ret;

		if (c->vsim_18v)
			ret = twl_mmc_shutdown_regulator(VSIM_DEV_GRP);
	}

	return ret;
}

static int twl_mmc1_set_power(struct device *dev, int slot, int power_on,
				int vdd)
{
	u32 reg;
	int ret = 0;
	struct twl_mmc_controller *c = &hsmmc[0];
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/*
	 * Assume we power both OMAP VMMC1 (for CMD, CLK, DAT0..3) and the
	 * card using the same TWL VMMC1 supply (hsmmc[0]); OMAP has both
	 * 1.8V and 3.0V modes, controlled by the PBIAS register.
	 *
	 * In 8-bit modes, OMAP VMMC1A (for DAT4..7) needs a supply, which
	 * is most naturally TWL VSIM; those pins also use PBIAS.
	 */
	if (power_on) {
		if (cpu_is_omap2430()) {
			reg = omap_ctrl_readl(OMAP243X_CONTROL_DEVCONF1);
			if ((1 << vdd) >= MMC_VDD_30_31)
				reg |= OMAP243X_MMC1_ACTIVE_OVERWRITE;
			else
				reg &= ~OMAP243X_MMC1_ACTIVE_OVERWRITE;
			omap_ctrl_writel(reg, OMAP243X_CONTROL_DEVCONF1);
		}

		if (mmc->slots[0].internal_clock) {
			reg = omap_ctrl_readl(OMAP2_CONTROL_DEVCONF0);
			reg |= OMAP2_MMCSDIO1ADPCLKISEL;
			omap_ctrl_writel(reg, OMAP2_CONTROL_DEVCONF0);
		}

		reg = omap_ctrl_readl(control_pbias_offset);
		reg |= OMAP2_PBIASSPEEDCTRL0;
		reg &= ~OMAP2_PBIASLITEPWRDNZ0;
		omap_ctrl_writel(reg, control_pbias_offset);

		ret = twl_mmc_set_voltage(c, vdd);

		/* 100ms delay required for PBIAS configuration */
		msleep(100);
		reg = omap_ctrl_readl(control_pbias_offset);
		reg |= (OMAP2_PBIASLITEPWRDNZ0 | OMAP2_PBIASSPEEDCTRL0);
		if ((1 << vdd) <= MMC_VDD_165_195)
			reg &= ~OMAP2_PBIASLITEVMODE0;
		else
			reg |= OMAP2_PBIASLITEVMODE0;
		omap_ctrl_writel(reg, control_pbias_offset);
	} else {
		reg = omap_ctrl_readl(control_pbias_offset);
		reg &= ~OMAP2_PBIASLITEPWRDNZ0;
		omap_ctrl_writel(reg, control_pbias_offset);

		ret = twl_mmc_set_voltage(c, 0);

		/* 100ms delay required for PBIAS configuration */
		msleep(100);
		reg = omap_ctrl_readl(control_pbias_offset);
		reg |= (OMAP2_PBIASSPEEDCTRL0 | OMAP2_PBIASLITEPWRDNZ0 |
			OMAP2_PBIASLITEVMODE0);
		omap_ctrl_writel(reg, control_pbias_offset);
	}

	return ret;
}

static int twl_mmc2_set_power(struct device *dev, int slot, int power_on, int vdd)
{
	int ret;
	struct twl_mmc_controller *c = &hsmmc[1];
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/*
	 * Assume TWL VMMC2 (hsmmc[1]) is used only to power the card ... OMAP
	 * VDDS is used to power the pins, optionally with a transceiver to
	 * support cards using voltages other than VDDS (1.8V nominal).  When a
	 * transceiver is used, DAT3..7 are muxed as transceiver control pins.
	 */
	if (power_on) {
		if (mmc->slots[0].internal_clock) {
			u32 reg;

			reg = omap_ctrl_readl(control_devconf1_offset);
			reg |= OMAP2_MMCSDIO2ADPCLKISEL;
			omap_ctrl_writel(reg, control_devconf1_offset);
		}
		ret = twl_mmc_set_voltage(c, vdd);
	} else {
		ret = twl_mmc_set_voltage(c, 0);
	}

	return ret;
}

static struct omap_mmc_platform_data *hsmmc_data[OMAP34XX_NR_MMC] __initdata;

void __init twl4030_mmc_init(struct twl4030_hsmmc_info *controllers)
{
	struct twl4030_hsmmc_info *c;
	int nr_hsmmc = ARRAY_SIZE(hsmmc_data);

	hsmmc_reset();
	if (cpu_is_omap2430()) {
		control_pbias_offset = OMAP243X_CONTROL_PBIAS_LITE;
		control_devconf1_offset = OMAP243X_CONTROL_DEVCONF1;
		nr_hsmmc = 2;
	} else {
		control_pbias_offset = OMAP343X_CONTROL_PBIAS_LITE;
		control_devconf1_offset = OMAP343X_CONTROL_DEVCONF1;
	}

	for (c = controllers; c->mmc; c++) {
		struct twl_mmc_controller *twl = hsmmc + c->mmc - 1;
		struct omap_mmc_platform_data *mmc = hsmmc_data[c->mmc - 1];

		if (!c->mmc || c->mmc > nr_hsmmc) {
			pr_debug("MMC%d: no such controller\n", c->mmc);
			continue;
		}
		if (mmc) {
			pr_debug("MMC%d: already configured\n", c->mmc);
			continue;
		}

		mmc = kzalloc(sizeof(struct omap_mmc_platform_data), GFP_KERNEL);
		if (!mmc) {
			pr_err("Cannot allocate memory for mmc device!\n");
			return;
		}

		if (c->name)
			strncpy(twl->name, c->name, HSMMC_NAME_LEN);
		else
			sprintf(twl->name, "mmc%islot%i", c->mmc, 1);
		mmc->slots[0].name = twl->name;
		mmc->nr_slots = 1;
		mmc->slots[0].wires = c->wires;
		mmc->slots[0].internal_clock = !c->ext_clock;
		mmc->dma_mask = 0xffffffff;

		/* note: twl4030 card detect GPIOs normally switch VMMCx ... */
		if (gpio_is_valid(c->gpio_cd)) {
			mmc->init = twl_mmc_late_init;
			mmc->cleanup = twl_mmc_cleanup;
			mmc->suspend = twl_mmc_suspend;
			mmc->resume = twl_mmc_resume;

			mmc->slots[0].switch_pin = c->gpio_cd;
			mmc->slots[0].card_detect_irq = gpio_to_irq(c->gpio_cd);
			if (c->cover_only)
				mmc->slots[0].get_cover_state = twl_mmc_get_cover_state;
			else
				mmc->slots[0].card_detect = twl_mmc_card_detect;
		} else
			mmc->slots[0].switch_pin = -EINVAL;

		/* write protect normally uses an OMAP gpio */
		if (gpio_is_valid(c->gpio_wp)) {
			gpio_request(c->gpio_wp, "mmc_wp");
			gpio_direction_input(c->gpio_wp);

			mmc->slots[0].gpio_wp = c->gpio_wp;
			mmc->slots[0].get_ro = twl_mmc_get_ro;
		} else
			mmc->slots[0].gpio_wp = -EINVAL;

		/* NOTE:  we assume OMAP's MMC1 and MMC2 use
		 * the TWL4030's VMMC1 and VMMC2, respectively;
		 * and that OMAP's MMC3 isn't used.
		 */

		switch (c->mmc) {
		case 1:
			mmc->slots[0].set_power = twl_mmc1_set_power;
			mmc->slots[0].ocr_mask = MMC1_OCR;
			break;
		case 2:
			mmc->slots[0].set_power = twl_mmc2_set_power;
			if (c->vmmc_dev_grp)
				twl->twl_vmmc_dev_grp = c->vmmc_dev_grp;
			if (c->transceiver)
				mmc->slots[0].ocr_mask = MMC2_OCR;
			else if (c->vsim_18v) {
				mmc->slots[0].ocr_mask = MMC_VDD_27_28 |
					MMC_VDD_28_29 | MMC_VDD_29_30 |
					MMC_VDD_30_31 | MMC_VDD_31_32;
				twl->vsim_18v = true;
			} else
				mmc->slots[0].ocr_mask = MMC_VDD_165_195;
			break;
		default:
			pr_err("MMC%d configuration not supported!\n", c->mmc);
			continue;
		}
		hsmmc_data[c->mmc - 1] = mmc;
	}

	omap2_init_mmc(hsmmc_data, OMAP34XX_NR_MMC);
}

#endif
