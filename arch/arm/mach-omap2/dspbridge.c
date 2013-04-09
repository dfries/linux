/*
 * TI's dspbridge platform device registration
 *
 * Copyright (C) 2005-2006 Texas Instruments, Inc.
 * Copyright (C) 2009 Nokia Corporation
 *
 * Written by Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>

#include <mach/omap-pm.h>

#include "omap3-opp.h"

#include <dspbridge/host_os.h>

#define BRIDGE_THRESH_HIGH_PERCENT	95
#define BRIDGE_THRESH_LOW_PERCENT	88

static struct platform_device *dspbridge_pdev;
static int update_mpu_speeds(struct dspbridge_platform_data *pdata);
static u8 dspbridge_get_opp_for_freq(unsigned long f);

static struct dspbridge_platform_data dspbridge_pdata __initdata = {
	.dsp_set_min_opp	= omap_pm_dsp_set_min_opp,
	.dsp_get_opp		= omap_pm_dsp_get_opp,
	.cpu_set_freq		= omap_pm_cpu_set_freq,
	.cpu_get_freq		= omap_pm_cpu_get_freq,
	.dsp_get_opp_for_freq	= dspbridge_get_opp_for_freq,
};

static int update_mpu_speeds(struct dspbridge_platform_data *pdata);

static int dspbridge_policy_notification(struct notifier_block *op,
		unsigned long val, void *ptr)
{
	if(CPUFREQ_ADJUST == val)
		update_mpu_speeds(dspbridge_pdev->dev.platform_data);
	return 0;
}

static struct notifier_block iva_clk_policy_notifier = {
	.notifier_call = dspbridge_policy_notification,
	NULL,
};

struct omap_opp dsp_rate_table[] = {
	{0, 0, 0},
	{90000,  VDD1_OPP1,   0},
	{180000, VDD1_OPP2,   0},
	{360000, VDD1_OPP3,   0},
	{400000, VDD1_OPP4,   0},
	{430000, VDD1_OPP5,   0},
	{460000, VDD1_OPP6,   0},
	{480000, VDD1_OPP7,   0},
	{500000, VDD1_OPP8,   0},
	{520000, VDD1_OPP9,   0},
	{540000, VDD1_OPP10,  0},
	{560000, VDD1_OPP11,  0},
	{580000, VDD1_OPP12,  0},
	{600000, VDD1_OPP13,  0},
};

static u8 dspbridge_get_opp_for_freq(unsigned long f)
{
	u8 opp;
	struct dspbridge_platform_data * pdata=dspbridge_pdev->dev.platform_data;
	unsigned long dsp_freq=0;

	for(opp=1; opp<=pdata->mpu_num_speeds; opp++)
		if(pdata->mpu_speeds[opp]/1000 >= f)
		{
			dsp_freq = omap3_dsp_rate_table[opp].rate / 1000;
			break;
		}
	for(opp=1; opp<=pdata->dsp_num_speeds; opp++)
		if(dsp_rate_table[opp].rate >= dsp_freq)
			break;
	return opp;
}

static int update_mpu_speeds(struct dspbridge_platform_data *pdata)
{
#ifdef CONFIG_BRIDGE_DVFS
	int mpu_freqs;
	int dsp_freqs;
	int i;
	unsigned long old_rate;
	struct cpufreq_policy policy;
	i = cpufreq_get_policy(&policy,0);
	if(i)
	{
		pr_err("%s cpufreq_get_policy failed %d\n",__func__,i);
		return i;
	}

	mpu_freqs = MAX_VDD1_OPP;
	dsp_freqs = VDD1_OPP13;
	if (mpu_freqs < 0 || dsp_freqs < 0 || mpu_freqs != dsp_freqs) {
		pr_err("%s:mpu and dsp frequencies are inconsistent! "
			"mpu_freqs=%d dsp_freqs=%d\n", __func__, mpu_freqs,
			dsp_freqs);
		return -EINVAL;
	}
	
	kfree(pdata->mpu_speeds);
	kfree(pdata->dsp_freq_table);
	pdata->mpu_speeds = NULL;
	pdata->dsp_freq_table = NULL;

	/* allocate memory if we have opps initialized */
	pdata->mpu_speeds = kzalloc(sizeof(u32) * mpu_freqs,
			GFP_KERNEL);
	if (!pdata->mpu_speeds) {
		pr_err("%s:unable to allocate memory for the mpu"
			"frequencies\n", __func__);
		return -ENOMEM;
	}
	
	/* Walk through allowed frequencies and buid table*/
	pdata->mpu_max_opp = mpu_freqs;
	for(i=1;i<=mpu_freqs;i++)
	{
		pdata->mpu_speeds[i] = omap3_mpu_rate_table[i].rate;
		if((pdata->mpu_speeds[i] >= policy.max*1000) && (pdata->mpu_max_opp == mpu_freqs))
			pdata->mpu_max_opp=i;
	}
	
	pdata->mpu_num_speeds = mpu_freqs;
	pdata->mpu_min_speed = policy.min*1000;
	pdata->mpu_max_speed = policy.max*1000;
	
	/* need an initial terminator */
	
	pdata->dsp_freq_table = kzalloc(
			sizeof(struct dsp_shm_freq_table) *
			(dsp_freqs+1 ), GFP_KERNEL);
	if (!pdata->dsp_freq_table) {
		pr_err("%s: unable to allocate memory for the dsp"
			"frequencies\n", __func__);
		return -ENOMEM;
	}
	old_rate = 0;
	
	for(i=1;i<=dsp_freqs;i++) {
		/* dsp frequencies are in khz */
		u32 rate = dsp_rate_table[i].rate;
    
		/*
		 * On certain 34xx silicons, certain OPPs are duplicated
		 * for DSP - handle those by copying previous opp value
		 */
		if (rate == old_rate) {
			memcpy(&pdata->dsp_freq_table[i],
				&pdata->dsp_freq_table[i-1],
				sizeof(struct dsp_shm_freq_table));
		} else {
			pdata->dsp_freq_table[i].dsp_freq = rate;
			pdata->dsp_freq_table[i].u_volts =
				dsp_rate_table[i].vsel;
			/*
			 * min threshold:
			 * NOTE: index 1 needs a min of 0! else no
			 * scaling happens at DSP!
			 */
			pdata->dsp_freq_table[i].thresh_min_freq =
				((old_rate * BRIDGE_THRESH_LOW_PERCENT) / 100);
    
			/* max threshold */
			pdata->dsp_freq_table[i].thresh_max_freq =
				((rate * BRIDGE_THRESH_HIGH_PERCENT) / 100);
		}
		old_rate = rate;
	}
	/* the last entry should map with maximum rate */
	pdata->dsp_freq_table[i - 1].thresh_max_freq = old_rate;
	
	pdata->dsp_num_speeds = dsp_freqs;
#endif
	return 0;
}

static int __init get_opp_table(struct dspbridge_platform_data *pdata)
{
#ifdef CONFIG_BRIDGE_DVFS
	return update_mpu_speeds(pdata);
#else
	return 0;
#endif
}
static int __init dspbridge_init(void)
{
	struct platform_device *pdev;
	int err = -ENOMEM;
	struct dspbridge_platform_data *pdata = &dspbridge_pdata;

	pdata->phys_mempool_base = dspbridge_get_mempool_base();

	if (pdata->phys_mempool_base) {
		pdata->phys_mempool_size = CONFIG_BRIDGE_MEMPOOL_SIZE;
		pr_info("%s: %x bytes @ %x\n", __func__,
			pdata->phys_mempool_size, pdata->phys_mempool_base);
	}

	pdev = platform_device_alloc("C6410", -1);
	if (!pdev)
		goto err_out;

	err = get_opp_table(pdata);
	if (err)
		goto err_out;

	err = platform_device_add_data(pdev, pdata, sizeof(*pdata));
	if (err)
		goto err_out;

	err = platform_device_add(pdev);
	if (err)
		goto err_out;

	dspbridge_pdev = pdev;
	if (cpufreq_register_notifier(&iva_clk_policy_notifier,
					CPUFREQ_POLICY_NOTIFIER))
		pr_err("%s: cpufreq_register_notifier failed for "
		       "iva2_ck\n", __func__);
	
	return 0;

err_out:
	platform_device_put(pdev);
	return err;
}
module_init(dspbridge_init);

static void __exit dspbridge_exit(void)
{
	struct dspbridge_platform_data *pdata = &dspbridge_pdata;
	if (cpufreq_unregister_notifier(&iva_clk_policy_notifier,
						CPUFREQ_POLICY_NOTIFIER))
		pr_err("%s: cpufreq_unregister_notifier failed for iva2_ck\n",
			__func__);
	kfree(pdata->mpu_speeds);
	kfree(pdata->dsp_freq_table);
	pdata->mpu_speeds = NULL;
	pdata->dsp_freq_table = NULL;
	platform_device_unregister(dspbridge_pdev);
}
module_exit(dspbridge_exit);

MODULE_AUTHOR("Hiroshi DOYU");
MODULE_DESCRIPTION("TI's dspbridge platform device registration");
MODULE_LICENSE("GPL v2");
