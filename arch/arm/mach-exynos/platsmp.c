/* linux/arch/arm/mach-exynos4/platsmp.c
 *
 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Cloned from linux/arch/arm/mach-vexpress/platsmp.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>
#include <linux/io.h>

#include <asm/cacheflush.h>
#include <asm/hardware/gic.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#include <mach/hardware.h>
#include <mach/regs-clock.h>
#include <mach/regs-pmu.h>

#include <plat/cpu.h>

extern void exynos4_secondary_startup(void);

#define CPU1_BOOT_REG		(samsung_rev() == EXYNOS4210_REV_1_1 ? \
				S5P_INFORM5 : S5P_VA_SYSRAM)


volatile int pen_release = -1;

static void write_pen_release(int val)
{
	pen_release = val;
	smp_wmb();
	__cpuc_flush_dcache_area((void *)&pen_release, sizeof(pen_release));
	outer_clean_range(__pa(&pen_release), __pa(&pen_release + 1));
}

static void __iomem *scu_base_addr(void)
{
	return (void __iomem *)(S5P_VA_SCU);
}

static DEFINE_SPINLOCK(boot_lock);

void platform_secondary_init(unsigned int cpu)
{
	gic_secondary_init(0);

	write_pen_release(-1);

	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}

int boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;

	spin_lock(&boot_lock);

	write_pen_release(cpu_logical_map(cpu));

	if (!(__raw_readl(S5P_ARM_CORE1_STATUS) & S5P_CORE_LOCAL_PWR_EN)) {
		__raw_writel(S5P_CORE_LOCAL_PWR_EN,
			     S5P_ARM_CORE1_CONFIGURATION);

		timeout = 10;

		
		while ((__raw_readl(S5P_ARM_CORE1_STATUS)
			& S5P_CORE_LOCAL_PWR_EN) != S5P_CORE_LOCAL_PWR_EN) {
			if (timeout-- == 0)
				break;

			mdelay(1);
		}

		if (timeout == 0) {
			printk(KERN_ERR "cpu1 power enable failed");
			spin_unlock(&boot_lock);
			return -ETIMEDOUT;
		}
	}

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		smp_rmb();

		__raw_writel(virt_to_phys(exynos4_secondary_startup),
			CPU1_BOOT_REG);
		gic_raise_softirq(cpumask_of(cpu), 1);

		if (pen_release == -1)
			break;

		udelay(10);
	}

	spin_unlock(&boot_lock);

	return pen_release != -1 ? -ENOSYS : 0;
}


void __init smp_init_cpus(void)
{
	void __iomem *scu_base = scu_base_addr();
	unsigned int i, ncores;

	if (soc_is_exynos5250())
		ncores = 2;
	else
		ncores = scu_base ? scu_get_core_count(scu_base) : 1;

	
	if (ncores > nr_cpu_ids) {
		pr_warn("SMP: %u cores greater than maximum (%u), clipping\n",
			ncores, nr_cpu_ids);
		ncores = nr_cpu_ids;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);

	set_smp_cross_call(gic_raise_softirq);
}

void __init platform_smp_prepare_cpus(unsigned int max_cpus)
{
	if (!soc_is_exynos5250())
		scu_enable(scu_base_addr());

	__raw_writel(virt_to_phys(exynos4_secondary_startup),
			CPU1_BOOT_REG);
}
