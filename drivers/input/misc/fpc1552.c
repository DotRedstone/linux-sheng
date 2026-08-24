// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fingerprint Cards FPC1552 platform resource driver
 *
 * The sensor protocol and biometric matching run in Qualcomm secure world.
 * This driver only owns the Linux-visible regulator, reset and interrupt
 * resources, matching the role of Xiaomi's downstream fpc1552 driver.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/regulator/consumer.h>

#define FPC1552_WAKEUP_MS	2000

struct fpc1552_data {
	struct device *dev;
	struct regulator *vdd;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	struct mutex lock; /* Serializes regulator and reset transitions. */
	atomic64_t irq_count;
	atomic_t irq_pending;
	int irq;
	bool powered;
	bool prepared;
	bool fingerdown_wait;
	bool wakeup_enabled;
};

static int fpc1552_parse_enable(const char *buf, bool *enable)
{
	if (sysfs_streq(buf, "enable")) {
		*enable = true;
		return 0;
	}
	if (sysfs_streq(buf, "disable")) {
		*enable = false;
		return 0;
	}

	return kstrtobool(buf, enable);
}

static int fpc1552_hw_reset(struct fpc1552_data *fpc)
{
	if (!fpc->powered)
		return -EHOSTDOWN;

	/* Preserve the timing sequence used by Xiaomi's downstream driver. */
	gpiod_set_value_cansleep(fpc->reset_gpio, 1);
	usleep_range(100, 200);
	gpiod_set_value_cansleep(fpc->reset_gpio, 0);
	usleep_range(5000, 5100);
	gpiod_set_value_cansleep(fpc->reset_gpio, 1);
	usleep_range(5000, 5100);

	return gpiod_get_value_cansleep(fpc->irq_gpio) < 0 ? -EIO : 0;
}

static int fpc1552_set_power(struct fpc1552_data *fpc, bool enable)
{
	int ret = 0;

	mutex_lock(&fpc->lock);

	if (enable == fpc->powered)
		goto out;

	if (enable) {
		ret = regulator_enable(fpc->vdd);
		if (ret)
			goto out;

		usleep_range(100, 1000);
		fpc->powered = true;
	} else {
		gpiod_set_value_cansleep(fpc->reset_gpio, 0);
		ret = regulator_disable(fpc->vdd);
		if (!ret) {
			fpc->powered = false;
			fpc->prepared = false;
		}
	}

out:
	mutex_unlock(&fpc->lock);
	return ret;
}

static irqreturn_t fpc1552_irq_thread(int irq, void *data)
{
	struct fpc1552_data *fpc = data;

	atomic64_inc(&fpc->irq_count);
	atomic_set(&fpc->irq_pending, 1);
	if (READ_ONCE(fpc->wakeup_enabled))
		pm_wakeup_event(fpc->dev, FPC1552_WAKEUP_MS);

	sysfs_notify(&fpc->dev->kobj, NULL, "irq");
	sysfs_notify(&fpc->dev->kobj, NULL, "irq_count");

	return IRQ_HANDLED;
}

static ssize_t hw_reset_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);
	bool reset;
	int ret;

	if (sysfs_streq(buf, "reset")) {
		reset = true;
	} else {
		ret = kstrtobool(buf, &reset);
		if (ret)
			return ret;
	}
	if (!reset)
		return count;

	mutex_lock(&fpc->lock);
	ret = fpc1552_hw_reset(fpc);
	mutex_unlock(&fpc->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(hw_reset);

static ssize_t irq_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);
	int value = gpiod_get_value_cansleep(fpc->irq_gpio);

	if (value < 0)
		return value;
	if (atomic_xchg(&fpc->irq_pending, 0))
		value = 1;

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t irq_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	if (!sysfs_streq(buf, "ack"))
		return -EINVAL;

	return count;
}
static DEVICE_ATTR_RW(irq);

static ssize_t irq_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&fpc->irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t wakeup_enable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(fpc->wakeup_enabled));
}

static ssize_t wakeup_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = fpc1552_parse_enable(buf, &enable);
	if (ret)
		return ret;

	if (enable != device_may_wakeup(dev)) {
		ret = device_set_wakeup_enable(dev, enable);
		if (ret)
			return ret;
	}

	WRITE_ONCE(fpc->wakeup_enabled, enable);
	return count;
}
static DEVICE_ATTR_RW(wakeup_enable);

static ssize_t device_prepare_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = fpc1552_parse_enable(buf, &enable);
	if (ret)
		return ret;

	ret = fpc1552_set_power(fpc, enable);
	if (ret || !enable)
		return ret ? ret : count;

	mutex_lock(&fpc->lock);
	ret = fpc1552_hw_reset(fpc);
	if (!ret)
		fpc->prepared = true;
	mutex_unlock(&fpc->lock);

	if (ret)
		fpc1552_set_power(fpc, false);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(device_prepare);

static ssize_t fingerdown_wait_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct fpc1552_data *fpc = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = fpc1552_parse_enable(buf, &enable);
	if (ret)
		return ret;
	if (!READ_ONCE(fpc->prepared))
		return -EHOSTDOWN;

	WRITE_ONCE(fpc->fingerdown_wait, enable);
	return count;
}
static DEVICE_ATTR_WO(fingerdown_wait);

static struct attribute *fpc1552_attrs[] = {
	&dev_attr_hw_reset.attr,
	&dev_attr_irq.attr,
	&dev_attr_irq_count.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_device_prepare.attr,
	&dev_attr_fingerdown_wait.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpc1552);

static void fpc1552_power_off(void *data)
{
	struct fpc1552_data *fpc = data;

	fpc1552_set_power(fpc, false);
}

static int fpc1552_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpc1552_data *fpc;
	int ret;

	fpc = devm_kzalloc(dev, sizeof(*fpc), GFP_KERNEL);
	if (!fpc)
		return -ENOMEM;

	fpc->dev = dev;
	fpc->wakeup_enabled = true;
	mutex_init(&fpc->lock);
	atomic64_set(&fpc->irq_count, 0);
	atomic_set(&fpc->irq_pending, 0);
	platform_set_drvdata(pdev, fpc);

	fpc->vdd = devm_regulator_get(dev, "fp_vdd_vreg");
	if (IS_ERR(fpc->vdd))
		return dev_err_probe(dev, PTR_ERR(fpc->vdd),
				     "failed to get sensor regulator\n");

	fpc->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(fpc->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc->reset_gpio),
				     "failed to get reset GPIO\n");

	fpc->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(fpc->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc->irq_gpio),
				     "failed to get interrupt GPIO\n");

	fpc->irq = platform_get_irq(pdev, 0);
	if (fpc->irq < 0)
		return fpc->irq;

	ret = devm_add_action_or_reset(dev, fpc1552_power_off, fpc);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, fpc->irq, NULL,
					fpc1552_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					dev_name(dev), fpc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	ret = device_init_wakeup(dev, true);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable wakeup\n");

	ret = dev_pm_set_wake_irq(dev, fpc->irq);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure wake IRQ\n");

	dev_info(dev, "FPC1552 resources ready, irq=%d level=%d\n", fpc->irq,
		 gpiod_get_value_cansleep(fpc->irq_gpio));
	return 0;
}

static void fpc1552_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
}

static const struct of_device_id fpc1552_of_match[] = {
	{ .compatible = "fpc,fpc1020" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc1552_of_match);

static struct platform_driver fpc1552_driver = {
	.probe = fpc1552_probe,
	.remove = fpc1552_remove,
	.driver = {
		.name = "fpc1552",
		.of_match_table = fpc1552_of_match,
		.dev_groups = fpc1552_groups,
	},
};
module_platform_driver(fpc1552_driver);

MODULE_DESCRIPTION("Fingerprint Cards FPC1552 platform resource driver");
MODULE_LICENSE("GPL");
