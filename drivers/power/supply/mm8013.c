// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Limited
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>

#define REG_BATID			0x00 /* This one is very unclear */
 #define BATID_101			0x0101 /* 107kOhm */
 #define BATID_102			0x0102 /* 10kOhm */
 #define BATID_103			0x0103 /* Lenovo Xiaoxin Pad Pro TB375FC */
#define REG_TEMPERATURE			0x06
#define REG_VOLTAGE			0x08
#define REG_FLAGS			0x0a
 #define MM8013_FLAG_OTC		BIT(15)
 #define MM8013_FLAG_OTD		BIT(14)
 #define MM8013_FLAG_BATHI		BIT(13)
 #define MM8013_FLAG_BATLOW		BIT(12)
 #define MM8013_FLAG_CHG_INH		BIT(11)
 #define MM8013_FLAG_FC			BIT(9)
 #define MM8013_FLAG_CHG		BIT(8)
 #define MM8013_FLAG_OCC		BIT(6)
 #define MM8013_FLAG_ODC		BIT(5)
 #define MM8013_FLAG_OT			BIT(4)
 #define MM8013_FLAG_UT			BIT(3)
 #define MM8013_FLAG_DSG		BIT(0)
#define REG_FULL_CHARGE_CAPACITY	0x0e
#define REG_NOMINAL_CHARGE_CAPACITY	0x0c
#define REG_AVERAGE_CURRENT		0x14
#define REG_AVERAGE_TIME_TO_EMPTY	0x16
#define REG_AVERAGE_TIME_TO_FULL	0x18
#define REG_MAX_LOAD_CURRENT		0x1e
#define REG_CYCLE_COUNT			0x2a
#define REG_STATE_OF_CHARGE		0x2c
#define REG_DESIGN_CAPACITY		0x3c
/* TODO: 0x62-0x68 seem to contain 'MM8013C' in a length-prefixed, non-terminated string */

#define DECIKELVIN_TO_DECIDEGC(t)	(t - 2731)

struct mm8013_chip {
	struct i2c_client *client;
};

/* MM8013 16-bit fields = two consecutive 8-bit registers, little-endian:
 * reg N = low byte, reg N+1 = high byte.  The SMBUS word path is unreliable
 * on the MTK adapter (2nd byte comes back 0x00/0xFF), so read each byte
 * separately and combine, retrying while a byte reads 0xFF (bus fill).
 */
static int mm8013_read_reg(struct mm8013_chip *chip, unsigned int reg, u32 *val)
{
	int lo, hi, retry;

	for (retry = 0; retry < 5; retry++) {
		lo = i2c_smbus_read_byte_data(chip->client, reg);
		if (lo < 0)
			return lo;
		hi = i2c_smbus_read_byte_data(chip->client, reg + 1);
		if (hi < 0)
			return hi;
		if (lo != 0xff && hi != 0xff) {
			*val = (hi << 8) | lo;
			return 0;
		}
		usleep_range(2000, 4000);
	}
	dev_err(&chip->client->dev, "MM8013 reg 0x%02X read unstable\n", reg);
	return -EIO;
}

static int mm8013_write_reg(struct mm8013_chip *chip, unsigned int reg, u16 val)
{
	return i2c_smbus_write_word_data(chip->client, reg, val);
}

static int mm8013_checkdevice(struct mm8013_chip *chip)
{
	int battery_id, ret;
	u32 val;

	ret = mm8013_write_reg(chip, REG_BATID, 0x0008);
	if (ret < 0)
		return ret;

	ret = mm8013_read_reg(chip, REG_BATID, &val);
	if (ret < 0)
		return ret;

	if (val == BATID_103)
		battery_id = 3;
	else if (val == BATID_102)
		battery_id = 2;
	else if (val == BATID_101)
		battery_id = 1;
	else {
		dev_warn(&chip->client->dev, "Unknown BATID 0x%04x, proceeding with battery_id=1\n", val);
		battery_id = 1;
	}

	dev_info(&chip->client->dev, "MM8013 detected with battery_id: %d (val=0x%04x)\n", battery_id, val);

	return 0;
}

static enum power_supply_property mm8013_battery_props[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static int mm8013_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct mm8013_chip *chip = power_supply_get_drvdata(psy);
	int ret = 0;
	u32 regval;

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = mm8013_read_reg(chip, REG_STATE_OF_CHARGE, &regval);
		if (ret < 0)
			return ret;

		val->intval = regval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = mm8013_read_reg(chip, REG_FULL_CHARGE_CAPACITY, &regval);
		if (ret < 0)
			return ret;

		val->intval = 1000 * regval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = mm8013_read_reg(chip, REG_DESIGN_CAPACITY, &regval);
		if (ret < 0)
			return ret;

		val->intval = 1000 * regval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = mm8013_read_reg(chip, REG_NOMINAL_CHARGE_CAPACITY, &regval);
		if (ret < 0)
			return ret;

		val->intval = 1000 * regval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = mm8013_read_reg(chip, REG_MAX_LOAD_CURRENT, &regval);
		if (ret < 0)
			return ret;

		val->intval = -1000 * (s16)regval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = mm8013_read_reg(chip, REG_AVERAGE_CURRENT, &regval);
		if (ret < 0)
			return ret;

		val->intval = -1000 * (s16)regval;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = mm8013_read_reg(chip, REG_CYCLE_COUNT, &regval);
		if (ret < 0)
			return ret;

		val->intval = regval;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = mm8013_read_reg(chip, REG_FLAGS, &regval);
		if (ret < 0)
			return ret;

		if (regval & MM8013_FLAG_UT)
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		else if (regval & (MM8013_FLAG_ODC | MM8013_FLAG_OCC))
			val->intval = POWER_SUPPLY_HEALTH_OVERCURRENT;
		else if (regval & (MM8013_FLAG_BATLOW))
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		else if (regval & MM8013_FLAG_BATHI)
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		else if (regval & (MM8013_FLAG_OT | MM8013_FLAG_OTD | MM8013_FLAG_OTC))
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = mm8013_read_reg(chip, REG_TEMPERATURE, &regval);
		if (ret < 0)
			return ret;

		val->intval = ((s16)regval > 0);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = mm8013_read_reg(chip, REG_FLAGS, &regval);
		if (ret < 0)
			return ret;

		if (regval & MM8013_FLAG_DSG)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (regval & MM8013_FLAG_CHG_INH)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (regval & MM8013_FLAG_CHG)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (regval & MM8013_FLAG_FC)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = mm8013_read_reg(chip, REG_TEMPERATURE, &regval);
		if (ret < 0)
			return ret;

		val->intval = DECIKELVIN_TO_DECIDEGC(regval);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		ret = mm8013_read_reg(chip, REG_AVERAGE_TIME_TO_EMPTY, &regval);
		if (ret < 0)
			return ret;

		/* The estimation is not yet ready */
		if (regval == U16_MAX)
			return -ENODATA;

		val->intval = regval;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		ret = mm8013_read_reg(chip, REG_AVERAGE_TIME_TO_FULL, &regval);
		if (ret < 0)
			return ret;

		/* The estimation is not yet ready */
		if (regval == U16_MAX)
			return -ENODATA;

		val->intval = regval;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = mm8013_read_reg(chip, REG_VOLTAGE, &regval);
		if (ret < 0)
			return ret;

		val->intval = 1000 * regval;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct power_supply_desc mm8013_desc = {
	.name			= "battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= mm8013_battery_props,
	.num_properties		= ARRAY_SIZE(mm8013_battery_props),
	.get_property		= mm8013_get_property,
};

static int mm8013_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev = &client->dev;
	struct power_supply *psy;
	struct mm8013_chip *chip;
	int ret = 0;

	if (!i2c_check_functionality(client->adapter,
					     I2C_FUNC_SMBUS_BYTE_DATA |
					     I2C_FUNC_SMBUS_WORD_DATA))
		return dev_err_probe(dev, -EIO,
				     "I2C_FUNC_SMBUS_BYTE/WORD_DATA not supported\n");

	chip = devm_kzalloc(dev, sizeof(struct mm8013_chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;

	ret = mm8013_checkdevice(chip);
	if (ret)
		return dev_err_probe(dev, ret, "MM8013 not found\n");

	psy_cfg.drv_data = chip;
	psy_cfg.fwnode = dev_fwnode(dev);

	psy = devm_power_supply_register(dev, &mm8013_desc, &psy_cfg);
	if (IS_ERR(psy))
		return PTR_ERR(psy);

	return 0;
}

static const struct i2c_device_id mm8013_id_table[] = {
	{ .name = "mm8013" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mm8013_id_table);

static const struct of_device_id mm8013_match_table[] = {
	{ .compatible = "mitsumi,mm8013" },
	{}
};

static struct i2c_driver mm8013_i2c_driver = {
	.probe = mm8013_probe,
	.id_table = mm8013_id_table,
	.driver = {
		.name = "mm8013",
		.of_match_table = mm8013_match_table,
	},
};
module_i2c_driver(mm8013_i2c_driver);

MODULE_DESCRIPTION("MM8013 fuel gauge driver");
MODULE_LICENSE("GPL");
