// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Amlogic Meson IR remote receiver
 *
 * Copyright (C) 2014 Beniamino Galvani <b.galvani@gmail.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/bitfield.h>

#include <media/rc-core.h>
#include <linux/amlogic/iomap.h>
#define DRIVER_NAME		"meson-ir"

/* valid on all Meson platforms */
#define IR_DEC_LDR_ACTIVE	0x00
#define IR_DEC_LDR_IDLE		0x04
#define IR_DEC_LDR_REPEAT	0x08
#define IR_DEC_BIT_0		0x0c
#define IR_DEC_REG0		0x10
#define IR_DEC_FRAME		0x14
#define IR_DEC_STATUS		0x18
#define IR_DEC_REG1		0x1c
/* only available on Meson 8b and newer */
#define IR_DEC_REG2		0x20

#define REG0_RATE_MASK		GENMASK(11, 0)

#define DECODE_MODE_NEC		0x0
#define DECODE_MODE_RAW		0x2

/* Meson 6b uses REG1 to configure the mode */
#define REG1_MODE_MASK		GENMASK(8, 7)
#define REG1_MODE_SHIFT		7

/* Meson 8b / GXBB use REG2 to configure the mode */
#define REG2_MODE_MASK		GENMASK(3, 0)
#define REG2_MODE_SHIFT		0

#define REG1_TIME_IV_MASK	GENMASK(28, 16)

#define REG1_IRQSEL_MASK	GENMASK(3, 2)
#define REG1_IRQSEL_NEC_MODE	0
#define REG1_IRQSEL_RISE_FALL	1
#define REG1_IRQSEL_FALL	2
#define REG1_IRQSEL_RISE	3

#define REG1_RESET		BIT(0)
#define REG1_POL		BIT(1)
#define REG1_ENABLE		BIT(15)

#define STATUS_IR_DEC_IN	BIT(8)

#define MESON_TRATE		10	/* us */

u32 backup_IR_DEC_REG0;
u32 backup_IR_DEC_REG1;
u32 backup_IR_DEC_LDR_ACTIVE;
u32 backup_IR_DEC_LDR_IDLE;
u32 backup_IR_DEC_BIT_0;
u32 backup_IR_DEC_LDR_REPEAT;

struct meson_ir {
	void __iomem	*reg;
	struct rc_dev	*rc;
	spinlock_t	lock;
	struct timer_list flush_timer;
	int irq;
};

static u32 meson_ir_get_reg(struct meson_ir *ir, unsigned int reg)
{
	return readl(ir->reg + reg);
}

static void meson_ir_set_mask(struct meson_ir *ir, unsigned int reg,
			      u32 mask, u32 value)
{
	u32 data = meson_ir_get_reg(ir, reg);

	data &= ~mask;
	data |= (value & mask);
	writel(data, ir->reg + reg);
}

static irqreturn_t meson_ir_irq(int irqno, void *dev_id)
{
	struct meson_ir *ir = dev_id;

	spin_lock(&ir->lock);

	ir_raw_event_store_edge(ir->rc,
		(readl(ir->reg + IR_DEC_STATUS) & STATUS_IR_DEC_IN)
		? true : false);

	mod_timer(&ir->flush_timer,
		jiffies + usecs_to_jiffies(ir->rc->timeout));

	ir_raw_event_handle(ir->rc);

	spin_unlock(&ir->lock);

	return IRQ_HANDLED;
}

static void flush_timer(struct timer_list *t)
{
	struct meson_ir *ir = from_timer(ir, t, flush_timer);
	struct ir_raw_event rawir = {};

	rawir.timeout = true;
	rawir.duration = ir->rc->timeout;
	ir_raw_event_store(ir->rc, &rawir);
	ir_raw_event_handle(ir->rc);
}

static void meson_ir_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson_ir *ir = platform_get_drvdata(pdev);
	struct device_node *node = dev->of_node;
	bool pulse_inverted = of_property_read_bool(node, "pulse-inverted");

	/* Reset the decoder */
	meson_ir_set_mask(ir, IR_DEC_REG1, REG1_RESET, REG1_RESET);
	meson_ir_set_mask(ir, IR_DEC_REG1, REG1_RESET, 0);

	/* Set general operation mode (= raw/software decoding) */
	if (of_device_is_compatible(node, "amlogic,meson6-ir"))
		meson_ir_set_mask(ir, IR_DEC_REG1, REG1_MODE_MASK,
					FIELD_PREP(REG1_MODE_MASK, DECODE_MODE_RAW));
	else
		meson_ir_set_mask(ir, IR_DEC_REG2, REG2_MODE_MASK,
					FIELD_PREP(REG2_MODE_MASK, DECODE_MODE_RAW));

	/* Set rate */
	meson_ir_set_mask(ir, IR_DEC_REG0, REG0_RATE_MASK, MESON_TRATE - 1);
	/* IRQ on rising and falling edges */
	meson_ir_set_mask(ir, IR_DEC_REG1, REG1_IRQSEL_MASK,
				FIELD_PREP(REG1_IRQSEL_MASK, REG1_IRQSEL_RISE_FALL));
	/* Set polarity Invert input polarity */
	meson_ir_set_mask(ir, IR_DEC_REG1, REG1_POL,
			pulse_inverted ? REG1_POL : 0);
	/* Enable the decoder */
	meson_ir_set_mask(ir, IR_DEC_REG1, REG1_ENABLE, REG1_ENABLE);
	/* read IR_DEC_STATUS and IR_DEC_FRAME to clear status */
	meson_ir_get_reg(ir, IR_DEC_STATUS);
	meson_ir_get_reg(ir, IR_DEC_FRAME);
}

static int meson_ir_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct resource *res;
	const char *map_name;
	struct meson_ir *ir;
	struct pinctrl *p;
	int ret;
	bool pulse_inverted = false;

	ir = devm_kzalloc(dev, sizeof(struct meson_ir), GFP_KERNEL);
	if (!ir)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ir->reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(ir->reg))
		return PTR_ERR(ir->reg);

	ir->irq = platform_get_irq(pdev, 0);
	if (ir->irq < 0)
		return ir->irq;

	ir->rc = devm_rc_allocate_device(dev, RC_DRIVER_IR_RAW);
	if (!ir->rc) {
		dev_err(dev, "failed to allocate rc device\n");
		return -ENOMEM;
	}

	ir->rc->priv = ir;
	ir->rc->device_name = DRIVER_NAME;
	ir->rc->input_phys = DRIVER_NAME "/input0";
	ir->rc->input_id.bustype = BUS_HOST;
	map_name = of_get_property(node, "linux,rc-map-name", NULL);
	ir->rc->map_name = map_name ? map_name : RC_MAP_EMPTY;
	ir->rc->allowed_protocols = RC_PROTO_BIT_ALL_IR_DECODER;
	ir->rc->rx_resolution = MESON_TRATE;
	ir->rc->min_timeout = 1;
	ir->rc->timeout = MS_TO_US(125);
	ir->rc->max_timeout = MS_TO_US(1250);
	ir->rc->driver_name = DRIVER_NAME;
	pulse_inverted = of_property_read_bool(node, "pulse-inverted");

	spin_lock_init(&ir->lock);
	platform_set_drvdata(pdev, ir);

	ret = devm_rc_register_device(dev, ir->rc);
	if (ret) {
		dev_err(dev, "failed to register rc device\n");
		return ret;
	}

	timer_setup(&ir->flush_timer, flush_timer, 0);

	ret = devm_request_irq(dev, ir->irq, meson_ir_irq, 0, NULL, ir);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	p = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(p)) {
		dev_err(dev, "pinctrl error, %ld\n", PTR_ERR(p));
		return -1;
	}

	meson_ir_init(pdev);

	// make register backup
	backup_IR_DEC_REG0 = meson_ir_get_reg(ir, IR_DEC_REG0);
	backup_IR_DEC_REG1 = meson_ir_get_reg(ir, IR_DEC_REG1);
	backup_IR_DEC_LDR_ACTIVE = meson_ir_get_reg(ir, IR_DEC_LDR_ACTIVE);
	backup_IR_DEC_LDR_IDLE = meson_ir_get_reg(ir, IR_DEC_LDR_IDLE);
	backup_IR_DEC_BIT_0 = meson_ir_get_reg(ir, IR_DEC_BIT_0);
	backup_IR_DEC_LDR_REPEAT = meson_ir_get_reg(ir, IR_DEC_LDR_REPEAT);

	dev_info(dev, "receiver initialized\n");

	return 0;
}

static int meson_ir_remove(struct platform_device *pdev)
{
	struct meson_ir *ir = platform_get_drvdata(pdev);
	unsigned long flags;

	/* Disable the decoder */
	spin_lock_irqsave(&ir->lock, flags);
	meson_ir_set_mask(ir, IR_DEC_REG1, REG1_ENABLE, 0);
	spin_unlock_irqrestore(&ir->lock, flags);

	del_timer_sync(&ir->flush_timer);

	return 0;
}

static void meson_ir_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct meson_ir *ir = platform_get_drvdata(pdev);
	unsigned long flags;

	spin_lock_irqsave(&ir->lock, flags);

	/*
	 * Set operation mode to NEC/hardware decoding to give
	 * bootloader a chance to power the system back on
	 */
	if (of_device_is_compatible(node, "amlogic,meson6-ir"))
		meson_ir_set_mask(ir, IR_DEC_REG1, REG1_MODE_MASK,
				  DECODE_MODE_NEC << REG1_MODE_SHIFT);
	else
		meson_ir_set_mask(ir, IR_DEC_REG2, REG2_MODE_MASK,
				  DECODE_MODE_NEC << REG2_MODE_SHIFT);

	/* Set rate to default value */
	meson_ir_set_mask(ir, IR_DEC_REG0, REG0_RATE_MASK, 0x13);

	spin_unlock_irqrestore(&ir->lock, flags);
}

#ifdef CONFIG_PM
static int meson_ir_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct meson_ir *ir = platform_get_drvdata(pdev);
	unsigned long flags;

	spin_lock_irqsave(&ir->lock, flags);

	// restore register backup
	meson_ir_set_mask(ir, IR_DEC_REG0, backup_IR_DEC_REG0, backup_IR_DEC_REG0);
	meson_ir_set_mask(ir, IR_DEC_REG1, backup_IR_DEC_REG1, backup_IR_DEC_REG1);
	meson_ir_set_mask(ir, IR_DEC_LDR_ACTIVE, backup_IR_DEC_LDR_ACTIVE, backup_IR_DEC_LDR_ACTIVE);
	meson_ir_set_mask(ir, IR_DEC_LDR_IDLE, backup_IR_DEC_LDR_IDLE, backup_IR_DEC_LDR_IDLE);
	meson_ir_set_mask(ir, IR_DEC_BIT_0, backup_IR_DEC_BIT_0, backup_IR_DEC_BIT_0);
	meson_ir_set_mask(ir, IR_DEC_LDR_REPEAT, backup_IR_DEC_LDR_REPEAT, backup_IR_DEC_LDR_REPEAT);

	meson_ir_init(pdev);
#ifndef CONFIG_AMLOGIC_MODIFY
	enable_irq_wake(ir->irq);
#else
	enable_irq(ir->irq);
#endif
	spin_unlock_irqrestore(&ir->lock, flags);

	dev_info(dev, "receiver resumed\n");

	return 0;
}

static int meson_ir_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct meson_ir *ir = platform_get_drvdata(pdev);
#ifndef CONFIG_AMLOGIC_MODIFY
	unsigned long flags;

	spin_lock_irqsave(&ir->lock, flags);
	disable_irq_wake(ir->irq);
	spin_unlock_irqrestore(&ir->lock, flags);
#else
	disable_irq(ir->irq);
#endif

	dev_info(dev, "receiver suspend\n");

	return 0;
}
#endif

static const struct of_device_id meson_ir_match[] = {
	{ .compatible = "amlogic,meson6-ir" },
	{ .compatible = "amlogic,meson8b-ir" },
	{ .compatible = "amlogic,meson-gxbb-ir" },
	{ },
};
MODULE_DEVICE_TABLE(of, meson_ir_match);

#ifdef CONFIG_PM
static const struct dev_pm_ops meson_ir_pm_ops = {
	.suspend_late = meson_ir_suspend,
	.resume_early = meson_ir_resume,
};
#endif

static struct platform_driver meson_ir_driver = {
	.probe		= meson_ir_probe,
	.remove		= meson_ir_remove,
	.shutdown	= meson_ir_shutdown,
	.driver = {
		.name		= DRIVER_NAME,
		.of_match_table	= meson_ir_match,
#ifdef CONFIG_PM
		.pm = &meson_ir_pm_ops,
#endif
	},
};

module_platform_driver(meson_ir_driver);

MODULE_DESCRIPTION("Amlogic Meson IR remote receiver driver");
MODULE_AUTHOR("Beniamino Galvani <b.galvani@gmail.com>");
MODULE_LICENSE("GPL v2");
