/* drivers/motor/isa1200_vibrator.c
 *
 * Copyright (C) 2011 Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/hrtimer.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/workqueue.h>

#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include <asm/mach-types.h>
#include <linux/isa1200_vibrator.h>

#include <linux/fs.h>
#include <asm/uaccess.h>

#include "../staging/android/timed_output.h"

#if 0
#define MOTOR_DEBUG
#endif

#define PWM_PERCENT_MIN     0
#define PWM_PERCENT_MAX     100


struct isa1200_vibrator_drvdata {
	struct timed_output_dev dev;
	struct hrtimer timer;
	struct work_struct work;
	struct clk *vib_clk;
	struct i2c_client *client;
	spinlock_t lock;
	bool running;
	int gpio_en;
	int timeout;
	int max_timeout;
	u8 ctrl0;
	u8 ctrl1;
	u8 ctrl2;
	u8 ctrl4;
	u8 pll;
	u8 duty;
	u8 period;
};

static int isa1200_config(struct isa1200_vibrator_drvdata *vib, int percent) {
	unsigned int period = vib->period;
	unsigned int duty =
			(period * (percent + PWM_PERCENT_MAX)) / (2 * PWM_PERCENT_MAX);

	vib->duty = duty;

	return 0;
}

static int isa1200_vibrator_i2c_write(struct i2c_client *client,
					u8 addr, u8 val)
{
	int error = 0;
	error = i2c_smbus_write_byte_data(client, addr, val);
	if (error)
		printk(KERN_ERR "[VIB] Failed to write addr=[0x%x], val=[0x%x]\n",
				addr, val);

	return error;
}

static void isa1200_vibrator_hw_init(struct isa1200_vibrator_drvdata *data)
{
	gpio_direction_output(data->gpio_en, 1);
	msleep(20);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_CONTROL_REG0, data->ctrl0);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_CONTROL_REG1, data->ctrl1);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_CONTROL_REG2, data->ctrl2);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_PLL_REG, data->pll);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_CONTROL_REG4, data->ctrl4);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_PWM_DUTY_REG, data->period/2);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_PWM_PERIOD_REG, data->period);

#ifdef MOTOR_DEBUG
	printk(KERN_DEBUG "[VIB] ctrl0 = 0x%x\n", data->ctrl0);
	printk(KERN_DEBUG "[VIB] ctrl1 = 0x%x\n", data->ctrl1);
	printk(KERN_DEBUG "[VIB] ctrl2 = 0x%x\n", data->ctrl2);
	printk(KERN_DEBUG "[VIB] pll = 0x%x\n", data->pll);
	printk(KERN_DEBUG "[VIB] ctrl4 = 0x%x\n", data->ctrl4);
	printk(KERN_DEBUG "[VIB] duty = 0x%x\n", data->period/2);
	printk(KERN_DEBUG "[VIB] period = 0x%x\n", data->period);
	printk(KERN_DEBUG "[VIB] gpio_en = 0x%x\n", data->gpio_en);
#endif

}

static void isa1200_vibrator_on(struct isa1200_vibrator_drvdata *data)
{
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_CONTROL_REG0, data->ctrl0 | CTL0_NORMAL_OP);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_PWM_DUTY_REG, data->duty - 3);
#ifdef MOTOR_DEBUG
	printk(KERN_DEBUG "[VIB] ctrl0 = 0x%x\n", data->ctrl0 | CTL0_NORMAL_OP);
	printk(KERN_DEBUG "[VIB] duty = 0x%x\n", data->duty);
#endif
}

static void isa1200_vibrator_off(struct isa1200_vibrator_drvdata *data)
{
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_PWM_DUTY_REG, data->period/2);
	isa1200_vibrator_i2c_write(data->client,
		HAPTIC_CONTROL_REG0, data->ctrl0);
}

static enum hrtimer_restart isa1200_vibrator_timer_func(struct hrtimer *_timer)
{
	struct isa1200_vibrator_drvdata *data =
		container_of(_timer, struct isa1200_vibrator_drvdata, timer);

	data->timeout = 0;

	schedule_work(&data->work);
	return HRTIMER_NORESTART;
}

static void isa1200_vibrator_work(struct work_struct *_work)
{
	struct isa1200_vibrator_drvdata *data =
		container_of(_work, struct isa1200_vibrator_drvdata, work);

	if (0 == data->timeout) {
		if (!data->running)
			return ;

		data->running = false;
		isa1200_vibrator_off(data);
		clk_disable_unprepare(data->vib_clk);
// #ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
// 		tegra_pinmux_set_tristate(TEGRA_PINGROUP_CDEV2,
// 				TEGRA_TRI_TRISTATE);
// #endif

	} else {
		if (data->running)
			return ;

		data->running = true;
// #ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
// 		tegra_pinmux_set_tristate(TEGRA_PINGROUP_CDEV2,
// 				TEGRA_TRI_NORMAL);
// #endif
		clk_prepare_enable(data->vib_clk);
		mdelay(1);
		isa1200_vibrator_on(data);
	}
}

static int isa1200_vibrator_get_time(struct timed_output_dev *_dev)
{
	struct isa1200_vibrator_drvdata	*data =
		container_of(_dev, struct isa1200_vibrator_drvdata, dev);

	if (hrtimer_active(&data->timer)) {
		ktime_t r = hrtimer_get_remaining(&data->timer);
		struct timeval t = ktime_to_timeval(r);
		return t.tv_sec * 1000 + t.tv_usec / 1000;
	} else
		return 0;
}

static void isa1200_vibrator_enable(struct timed_output_dev *_dev, int value)
{
	struct isa1200_vibrator_drvdata	*data =
		container_of(_dev, struct isa1200_vibrator_drvdata, dev);
	unsigned long	flags;

#ifdef MOTOR_DEBUG
	printk(KERN_DEBUG "[VIB] time = %dms\n", value);
#endif
	cancel_work_sync(&data->work);
	hrtimer_cancel(&data->timer);
	data->timeout = value;
	schedule_work(&data->work);
	spin_lock_irqsave(&data->lock, flags);
	if (value > 0) {
		if (value > data->max_timeout)
			value = data->max_timeout;

		hrtimer_start(&data->timer,
			ns_to_ktime((u64)value * NSEC_PER_MSEC),
			HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&data->lock, flags);
}

static ssize_t isa1200_pwm_min_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", PWM_PERCENT_MIN);
}

static ssize_t isa1200_pwm_max_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", PWM_PERCENT_MAX);
}

static ssize_t isa1000_pwm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1200_vibrator_drvdata *vib =
			container_of(timed_dev, struct isa1200_vibrator_drvdata, dev);

	int percent;

	// duty = (period * (percent + PWM_PERCENT_MAX)) / (2 * PWM_PERCENT_MAX);
	percent = ((vib->duty * (2 * PWM_PERCENT_MAX)) / vib->period) - PWM_PERCENT_MAX;

	return sprintf(buf, "%d\n", percent);
}

static ssize_t isa1200_pwm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1200_vibrator_drvdata *vib =
			container_of(timed_dev, struct isa1200_vibrator_drvdata, dev);
	int duty_percent;

	sscanf(buf, "%d", &duty_percent);

	if (duty_percent > PWM_PERCENT_MAX)
		duty_percent = PWM_PERCENT_MAX;
	else if (duty_percent < PWM_PERCENT_MIN)
		duty_percent = PWM_PERCENT_MIN;

	if (isa1200_config(vib, duty_percent) < 0) {
		pr_err("%s: failed to configure pwm\n", __func__);
	}

	return size;
}

static struct device_attribute isa1200_device_attrs[] = {
	__ATTR(duty_cycle_min, S_IRUGO,
			isa1200_pwm_min_show,
			NULL),
	__ATTR(duty_cycle_max, S_IRUGO,
			isa1200_pwm_max_show,
			NULL),
	__ATTR(duty_cycle, S_IRUGO | S_IWUSR,
			isa1000_pwm_show,
			isa1200_pwm_store),
};


#ifdef CONFIG_OF
static int isa1200_parse_dt(struct i2c_client *client,
		struct isa1200_vibrator_drvdata *drvdata)
{
	struct device_node *np = client->dev.of_node;
	struct clk *vib_clk;
	int val;

	val = of_get_gpio(np, 0);
	if (val < 0) {
		pr_err("%s: error(%d) parsing device tree gpio\n", __func__, val);
		return val;
	}
	drvdata->gpio_en = val;

	if (!of_property_read_u32(np, "max-timeout", &val))
		drvdata->max_timeout = val;
	if (!of_property_read_u32(np, "ctrl0", &val))
		drvdata->ctrl0 = val;
	if (!of_property_read_u32(np, "ctrl1", &val))
		drvdata->ctrl1 = val;
	if (!of_property_read_u32(np, "ctrl2", &val))
		drvdata->ctrl2 = val;
	if (!of_property_read_u32(np, "ctrl4", &val))
		drvdata->ctrl4 = val;
	if (!of_property_read_u32(np, "pll", &val))
		drvdata->pll = val;
	if (!of_property_read_u32(np, "duty", &val))
		drvdata->duty = val;
	if (!of_property_read_u32(np, "period", &val))
		drvdata->period = val;

	vib_clk = of_clk_get_by_name(np, "vibrator-clk");
	if (vib_clk == NULL) {
		pr_err("%s: error getting clk.\n", __func__);
		return -ENODEV;
	}
	drvdata->vib_clk = vib_clk;

	pr_info("%s: gpio_en=%d\n", __func__, drvdata->gpio_en);

	return 0;
}
#else
static int isa1200_parse_dt(struct i2c_client *client,
		struct isa1200_vibrator_drvdata *drvdata)
{
	return -EINVAL;
}
#endif

static int isa1200_vibrator_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct isa1200_vibrator_platform_data *pdata = NULL;
	struct isa1200_vibrator_drvdata *ddata;
	int i;
	int ret = 0;

	printk(KERN_DEBUG "[VIB] %s\n", __func__);

	ddata = kzalloc(sizeof(struct isa1200_vibrator_drvdata), GFP_KERNEL);
	if (NULL == ddata) {
		printk(KERN_ERR "[VIB] Failed to alloc memory\n");
		ret = -ENOMEM;
		goto err_free_mem;
	}

	if (client->dev.platform_data) {
		pdata = client->dev.platform_data;

		ddata->gpio_en = pdata->gpio_en;
		ddata->vib_clk = pdata->get_clk();
		ddata->max_timeout = pdata->max_timeout;
		ddata->ctrl0 = pdata->ctrl0;
		ddata->ctrl1 = pdata->ctrl1;
		ddata->ctrl2 = pdata->ctrl2;
		ddata->ctrl4 = pdata->ctrl4;
		ddata->pll = pdata->pll;
		ddata->duty = pdata->duty;
		ddata->period = pdata->period;
	} else if (client->dev.of_node) {
		ret = isa1200_parse_dt(client, ddata);
		if (ret) {
			pr_err("%s: error parsing device tree\n", __func__);
			goto err_free_mem;
		}
	}

	ddata->client = client;
	ddata->dev.name = "vibrator";
	ddata->dev.get_time = isa1200_vibrator_get_time;
	ddata->dev.enable = isa1200_vibrator_enable;

	hrtimer_init(&ddata->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ddata->timer.function = isa1200_vibrator_timer_func;
	INIT_WORK(&ddata->work, isa1200_vibrator_work);
	spin_lock_init(&ddata->lock);

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	ret = gpio_request(ddata->gpio_en, "vib_en");
	if (ret < 0) {
		pr_err("[VIB] Failed to request gpio %d\n", ddata->gpio_en);
		goto err_gpio_req2;
	}
#endif

	i2c_set_clientdata(client, ddata);
	isa1200_vibrator_hw_init(ddata);

	ret = timed_output_dev_register(&ddata->dev);
	if (ret < 0) {
		printk(KERN_ERR "[VIB] Failed to register timed_output : -%d\n", ret);
		goto err_to_dev_reg;
	}

	for (i = 0; i < ARRAY_SIZE(isa1200_device_attrs); i++) {
		ret = device_create_file(&client->dev, &isa1200_device_attrs[i]);
		if (ret < 0) {
			dev_err(&client->dev,
					"%s: failed to create sysfs attributes\n", __func__);
			goto err_to_dev_reg;
		}
	}

	return 0;

err_to_dev_reg:
	gpio_free(ddata->gpio_en);
err_gpio_req2:
err_free_mem:
	kfree(ddata);
	return ret;

}

static int isa1200_vibrator_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct isa1200_vibrator_drvdata *ddata  = i2c_get_clientdata(client);
	gpio_direction_output(ddata->gpio_en, 0);
	return 0;
}

static int isa1200_vibrator_resume(struct i2c_client *client)
{
	struct isa1200_vibrator_drvdata *ddata  = i2c_get_clientdata(client);
	isa1200_vibrator_hw_init(ddata);
	return 0;
}

static const struct i2c_device_id isa1200_vibrator_device_id[] = {
	{"isa1200_vibrator", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, isa1200_vibrator_device_id);

#ifdef CONFIG_OF
static const struct of_device_id isa1200_dt_match[] = {
	{ .compatible = "samsung_p3,isa1200_vibrator" },
	{ },
};
MODULE_DEVICE_TABLE(of, isa1200_dt_match);
#endif

static struct i2c_driver isa1200_vibrator_i2c_driver = {
	.driver = {
		.name = "isa1200_vibrator",
		.of_match_table = of_match_ptr(isa1200_dt_match),
		.owner = THIS_MODULE,
	},
	.probe     = isa1200_vibrator_i2c_probe,
	.id_table  = isa1200_vibrator_device_id,
};

module_i2c_driver(isa1200_vibrator_i2c_driver);
