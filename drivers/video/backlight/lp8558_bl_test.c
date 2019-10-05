// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>

/* LP8550/1/2/3/6 Registers */
#define LP855X_BRIGHTNESS_CTRL		0x00
#define LP855X_DEVICE_CTRL		0x01
#define LP8558_EEPROM_START		0x98
#define LP8558_EEPROM_END		0xAF

static void lp8558_dump(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	u8 reg;

	dev_err(dev, "Brightness: %d, Device Control: %d\n",
		i2c_smbus_read_byte_data(client, LP855X_BRIGHTNESS_CTRL),
		i2c_smbus_read_byte_data(client, LP855X_DEVICE_CTRL));

	for (reg = LP8558_EEPROM_START; reg <= LP8558_EEPROM_END; ++reg) {
		dev_err(dev, "EEPROM(%x) = 0x%x\n", reg,
			i2c_smbus_read_byte_data(client, reg));
	}
}

static int lp8558_probe(struct i2c_client *client)
{
	struct gpio_desc *enable;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EIO;

	enable = devm_gpiod_get(&client->dev, "enable", GPIOD_ASIS);
	if (IS_ERR(enable)) {
		return PTR_ERR(enable);
	}

	lp8558_dump(client);

	/* Disable backlight */
	gpiod_set_value_cansleep(enable, 0);
	msleep(2000);

	/* Enable backlight */
	gpiod_set_value_cansleep(enable, 1);
	msleep(1);
	lp8558_dump(client);

	/* Disable backlight again */
	gpiod_set_value_cansleep(enable, 0);

	return -ENODEV;
}

static const struct of_device_id lp8558_dt_ids[] = {
	{ .compatible = "ti,lp8558-test", },
	{ }
};
//MODULE_DEVICE_TABLE(of, lp8558_dt_ids);

static struct i2c_driver lp8558_driver = {
	.driver = {
		.name = "lp8558",
		.of_match_table = of_match_ptr(lp8558_dt_ids),
	},
	.probe_new = lp8558_probe,
};
module_i2c_driver(lp8558_driver);

MODULE_DESCRIPTION("Texas Instruments LP8558 Backlight test driver");
MODULE_LICENSE("GPL");
