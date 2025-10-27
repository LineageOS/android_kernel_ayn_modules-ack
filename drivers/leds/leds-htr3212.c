// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for HEROIC HTR3212 12-channel 8-bit PWM LED controller
 *
 * Copyright (c) 2025 Teguh Sobirin <teguh@sobir.in>
 *
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define HTR3212_CHANNELS 12
#define HTR3212_SECTIONS (HTR3212_CHANNELS / 3)
#define HTR3212_ENABLE_BIT 1
#define HTR3212_SHUTDOWN 0x00
#define HTR3212_PWM_REGISTER_BASE 0x0d
#define HTR3212_PWM_UPDATE 0x25
#define HTR3212_LED_CONTROL_BASE 0x32
#define HTR3212_GLOBAL_CONTROL 0x4a
#define HTR3212_OUTPUT_FREQ 0x4b
#define HTR3212_RESET 0x4f

struct htr3212_priv;

struct htr3212_led_data {
	struct led_classdev_mc mcled_cdev;
	struct htr3212_priv *priv;
};

struct htr3212_priv {
	struct device *dev;
	struct gpio_desc *sdb;
	struct regulator *vdd;
	struct regmap *regmap;
	struct mutex lock;
	struct htr3212_led_data leds[HTR3212_SECTIONS];
	unsigned int num_leds;
};

static int htr3212_write(struct htr3212_priv *priv, u8 reg, u8 val)
{
	int ret;

	dev_dbg(priv->dev, "writing register 0x%02X=0x%02X", reg, val);

	mutex_lock(&priv->lock);
	ret = regmap_write(priv->regmap, reg, val);
	mutex_unlock(&priv->lock);
	if (ret) {
		dev_err(priv->dev,
			"register write to 0x%02X failed (error %d)",
			reg, ret);
	}
	return ret;
}

static int htr3212_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness)
{
	struct led_classdev_mc *mcled_cdev = lcdev_to_mccdev(led_cdev);
	const struct htr3212_led_data *led_data =
		container_of(mcled_cdev, struct htr3212_led_data, mcled_cdev);
	int ret, i;

	dev_dbg(led_data->priv->dev, "%s: %d\n", __func__, brightness);

	led_mc_calc_color_components(mcled_cdev, brightness);

	for (i = 0; i < mcled_cdev->num_colors; i++) {
		struct mc_subled *subled_info = &mcled_cdev->subled_info[i];

		ret = htr3212_write(led_data->priv,
				       HTR3212_PWM_REGISTER_BASE + subled_info->channel,
				       subled_info->brightness);
		if (ret)
			return ret;
	}

	return htr3212_write(led_data->priv, HTR3212_PWM_UPDATE, 0x00);
}

static int htr3212_reset_regs(struct htr3212_priv *priv)
{
	int ret;

	ret = htr3212_write(priv, HTR3212_RESET, 0x00);
	if (ret)
		return ret;

	return 0;
}

static int htr3212_init_regs(struct htr3212_priv *priv)
{
	int ret;

	ret = htr3212_reset_regs(priv);
	if (ret)
		return ret;

	u8 value = GENMASK(HTR3212_ENABLE_BIT, 0);
	u8 num_regs = HTR3212_CHANNELS / HTR3212_ENABLE_BIT;

	int i;

	for (i = 0; i < num_regs; i++) {
		ret = htr3212_write(priv,
				HTR3212_LED_CONTROL_BASE + i, value);
		if (ret)
			return ret;
	}

	ret = htr3212_write(priv, HTR3212_SHUTDOWN, 0x01);
	if (ret)
		return ret;

	ret = htr3212_write(priv, HTR3212_OUTPUT_FREQ, 0x01);
	if (ret)
		return ret;

	ret = htr3212_write(priv, HTR3212_GLOBAL_CONTROL, 0x00);
	if (ret)
		return ret;

	return 0;
}

static int htr3212_parse_child_dt(const struct device *dev,
				     const struct device_node *child,
				     struct htr3212_led_data *led_data)
{
	struct led_classdev_mc *mcled_cdev = &led_data->mcled_cdev;
	int ret = 0, led_count = 9;
	u32 reg, color;

	led_count = of_get_available_child_count(child);
	mcled_cdev->subled_info = devm_kcalloc(led_data->priv->dev, led_count,
					       sizeof(struct mc_subled), GFP_KERNEL);

	for_each_available_child_of_node_scoped(child, led_node) {
		ret = of_property_read_u32(led_node, "reg", &reg);
		if (ret || reg > HTR3212_CHANNELS) {
			dev_err(dev,
				"Child node %pOF does not have a valid reg property\n",
				led_node);
			return -EINVAL;
		}
		mcled_cdev->subled_info[mcled_cdev->num_colors].channel = reg;

		ret = of_property_read_u32(led_node, "color", &color);
		if (ret) {
			dev_err(dev,
				"Child node %pOF does not have a valid color property\n",
				led_node);
			return -EINVAL;
		}
		mcled_cdev->subled_info[mcled_cdev->num_colors].color_index = color;

		mcled_cdev->num_colors++;
	}

	mcled_cdev->led_cdev.brightness_set_blocking = htr3212_brightness_set;

	return 0;
}

static int htr3212_parse_dt(struct device *dev,
			       struct htr3212_priv *priv)
{
	int ret = 0;

	for_each_available_child_of_node_scoped(dev_of_node(dev), child) {
		struct led_init_data init_data = {};
		struct htr3212_led_data *led_data =
			&priv->leds[priv->num_leds];

		led_data->priv = priv;

		ret = htr3212_parse_child_dt(dev, child, led_data);
		if (ret)
			return ret;

		init_data.fwnode = of_fwnode_handle(child);

		ret = devm_led_classdev_multicolor_register_ext(dev, &led_data->mcled_cdev,
								&init_data);
		if (ret) {
			dev_err(dev, "Failed to register LED for %pOF: %d\n",
				child, ret);
			return ret;
		}

		priv->num_leds++;
	}

	return 0;
}

static const struct regmap_config htr3212_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = HTR3212_RESET,
	.cache_type = REGCACHE_NONE,
};

static int htr3212_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct htr3212_priv *priv;
	int count;
	int ret = 0;

	count = of_get_available_child_count(dev_of_node(dev));
	if (!count)
		return -EINVAL;

	priv = devm_kzalloc(dev, struct_size(priv, leds, count),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->sdb = devm_gpiod_get(dev, "sdb", GPIOD_OUT_HIGH);
	if (PTR_ERR(priv->sdb) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	priv->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(priv->vdd)) {
		ret = PTR_ERR(priv->vdd);
		return ret;
	}

	ret = regulator_enable(priv->vdd);
	if (ret < 0) {
		return ret;
	}

	gpiod_set_value_cansleep(priv->sdb, 1);
	usleep_range(10000, 11000);

	priv->dev = &client->dev;

	priv->regmap = devm_regmap_init_i2c(client, &htr3212_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		return ret;
	}

	i2c_set_clientdata(client, priv);
	mutex_init(&priv->lock);

	ret = htr3212_init_regs(priv);
	if (ret)
		return ret;

	ret = htr3212_parse_dt(dev, priv);
	if (ret)
		return ret;

	return 0;
}

static void htr3212_remove(struct i2c_client *client)
{
	struct htr3212_priv *priv = i2c_get_clientdata(client);
	int ret;

	ret = htr3212_reset_regs(priv);
	if (ret)
		dev_err(&client->dev, "Failed to reset registers on removal (%pe)\n",
			ERR_PTR(ret));

	gpiod_set_value_cansleep(priv->sdb, 0);

	regulator_disable(priv->vdd);
}

static const struct of_device_id of_htr3212_match[] = {
	{ .compatible = "heroic,htr3212", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, of_htr3212_match);

static const struct i2c_device_id htr3212_id[] = {
	{ "htr3212" },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(i2c, htr3212_id);

static struct i2c_driver htr3212_driver = {
	.driver = {
		.name	= "htr3212",
		.of_match_table = of_htr3212_match,
	},
	.probe		= htr3212_probe,
	.remove		= htr3212_remove,
	.id_table	= htr3212_id,
};

module_i2c_driver(htr3212_driver);

MODULE_AUTHOR("Teguh Sobirin <teguh@sobir.in");
MODULE_DESCRIPTION("HEROIC HTR3212 LED Driver");
MODULE_LICENSE("GPL v2");
