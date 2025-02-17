// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2002 ARM Limited, All Rights Reserved.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/arm-gic.h>

#include "irq-gic-common.h"

#if IS_ENABLED(CONFIG_AMLOGIC_FREERTOS)
#include <linux/amlogic/freertos.h>
#endif

static DEFINE_RAW_SPINLOCK(irq_controller_lock);

void gic_enable_of_quirks(const struct device_node *np,
			  const struct gic_quirk *quirks, void *data)
{
	for (; quirks->desc; quirks++) {
		if (!quirks->compatible && !quirks->property)
			continue;
		if (quirks->compatible &&
		    !of_device_is_compatible(np, quirks->compatible))
			continue;
		if (quirks->property &&
		    !of_property_read_bool(np, quirks->property))
			continue;
		if (quirks->init(data))
			pr_info("GIC: enabling workaround for %s\n",
				quirks->desc);
	}
}

void gic_enable_quirks(u32 iidr, const struct gic_quirk *quirks,
		void *data)
{
	for (; quirks->desc; quirks++) {
		if (quirks->compatible || quirks->property)
			continue;
		if (quirks->iidr != (quirks->mask & iidr))
			continue;
		if (quirks->init(data))
			pr_info("GIC: enabling workaround for %s\n",
				quirks->desc);
	}
}

int gic_configure_irq(unsigned int irq, unsigned int type,
		       void __iomem *base, void (*sync_access)(void))
{
	u32 confmask = 0x2 << ((irq % 16) * 2);
	u32 confoff = (irq / 16) * 4;
	u32 val, oldval;
	int ret = 0;
	unsigned long flags;

	/*
	 * Read current configuration register, and insert the config
	 * for "irq", depending on "type".
	 */
	raw_spin_lock_irqsave(&irq_controller_lock, flags);
	val = oldval = readl_relaxed(base + confoff);
	if (type & IRQ_TYPE_LEVEL_MASK)
		val &= ~confmask;
	else if (type & IRQ_TYPE_EDGE_BOTH)
		val |= confmask;

	/* If the current configuration is the same, then we are done */
	if (val == oldval) {
		raw_spin_unlock_irqrestore(&irq_controller_lock, flags);
		return 0;
	}

	/*
	 * Write back the new configuration, and possibly re-enable
	 * the interrupt. If we fail to write a new configuration for
	 * an SPI then WARN and return an error. If we fail to write the
	 * configuration for a PPI this is most likely because the GIC
	 * does not allow us to set the configuration or we are in a
	 * non-secure mode, and hence it may not be catastrophic.
	 */
	writel_relaxed(val, base + confoff);
	if (readl_relaxed(base + confoff) != val)
		ret = -EINVAL;

	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);

	if (sync_access)
		sync_access();

	return ret;
}

void gic_dist_config(void __iomem *base, int gic_irqs,
		     void (*sync_access)(void))
{
	unsigned int i;
#if IS_ENABLED(CONFIG_AMLOGIC_FREERTOS)
	u32 tmp;
#endif

	/*
	 * Set all global interrupts to be level triggered, active low.
	 */
#if IS_ENABLED(CONFIG_AMLOGIC_FREERTOS)
	for (i = 32; i < gic_irqs; i += 16) {
		tmp = readl_relaxed(base + GIC_DIST_CONFIG + i / 4);
		tmp = freertos_get_irqregval
			(GICD_INT_ACTLOW_LVLTRIG, tmp, i, 16);
		writel_relaxed(tmp, base + GIC_DIST_CONFIG + i / 4);
	}
#else
	for (i = 32; i < gic_irqs; i += 16)
		writel_relaxed(GICD_INT_ACTLOW_LVLTRIG,
					base + GIC_DIST_CONFIG + i / 4);
#endif

	/*
	 * Set priority on all global interrupts.
	 */
#if IS_ENABLED(CONFIG_AMLOGIC_FREERTOS)
	for (i = 32; i < gic_irqs; i += 4) {
		tmp = readl_relaxed(base + GIC_DIST_PRI + i);
		tmp = freertos_get_irqregval
			(GICD_INT_DEF_PRI_X4, tmp, i, 4);
		writel_relaxed(tmp, base + GIC_DIST_PRI + i);
	}
#else
	for (i = 32; i < gic_irqs; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4, base + GIC_DIST_PRI + i);
#endif

	/*
	 * Deactivate and disable all SPIs. Leave the PPI and SGIs
	 * alone as they are in the redistributor registers on GICv3.
	 */
#if IS_ENABLED(CONFIG_AMLOGIC_FREERTOS)
	for (i = 32; i < gic_irqs; i += 32) {
		writel_relaxed
			(freertos_get_irqregval(GICD_INT_EN_CLR_X32, 0, i, 32),
			 base + GIC_DIST_ACTIVE_CLEAR + i / 8);
		writel_relaxed
			(freertos_get_irqregval(GICD_INT_EN_CLR_X32, 0, i, 32),
			 base + GIC_DIST_ENABLE_CLEAR + i / 8);
	}
#else
	for (i = 32; i < gic_irqs; i += 32) {
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ACTIVE_CLEAR + i / 8);
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ENABLE_CLEAR + i / 8);
	}
#endif

	if (sync_access)
		sync_access();
}

void gic_cpu_config(void __iomem *base, int nr, void (*sync_access)(void))
{
	int i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * private interrupts. Make sure everything is deactivated.
	 */
	for (i = 0; i < nr; i += 32) {
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ACTIVE_CLEAR + i / 8);
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ENABLE_CLEAR + i / 8);
	}

	/*
	 * Set priority on PPI and SGI interrupts
	 */
	for (i = 0; i < nr; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4,
					base + GIC_DIST_PRI + i * 4 / 4);

	if (sync_access)
		sync_access();
}
