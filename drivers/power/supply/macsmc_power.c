// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC Power/Battery Management
 * Copyright The Asahi Linux Contributors
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/power_supply.h>

#define MAX_STRING_LENGTH 256

struct macsmc_power {
	struct device *dev;
	struct apple_smc *smc;
	struct power_supply *psy;
	char model_name[MAX_STRING_LENGTH];
	char serial_number[MAX_STRING_LENGTH];

	struct notifier_block nb;
};

static int macsmc_battery_get_status(struct macsmc_power *power)
{
	u8 val;
	int ret;
	
	ret = apple_smc_read_u8(power->smc, SMC_KEY(BSFC), &val);
	if (ret)
		return ret;
	if (val == 1)
		return POWER_SUPPLY_STATUS_FULL;

	ret = apple_smc_read_u8(power->smc, SMC_KEY(CHSC), &val);
	if (ret)
		return ret;
	if (val == 1)
		return POWER_SUPPLY_STATUS_CHARGING;

	ret = apple_smc_read_u8(power->smc, SMC_KEY(CHCC), &val);
	if (ret)
		return ret;
	if (val == 0)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	ret = apple_smc_read_u8(power->smc, SMC_KEY(CHCE), &val);
	if (ret)
		return ret;
	if (val == 0)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;

	
}

static int macsmc_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct macsmc_power *power = power_supply_get_drvdata(psy);
	int ret = 0;
	u16 vu16;
	u32 vu32;
	s16 vs16;
	s32 vs32;
	s64 vs64;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = macsmc_battery_get_status(power);
		ret = val->intval < 0 ? val->intval : 0;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0TE), &vu16);
		val->intval = vu16 == 0xffff ? 0 : vu16 * 60;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0TF), &vu16);
		val->intval = vu16 == 0xffff ? 0 : vu16 * 60;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(BRSC), &vu16);
		val->intval = vu16;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0AV), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = apple_smc_read_s16(power->smc, SMC_KEY(B0AC), &vs16);
		val->intval = vs16 * 1000;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		ret = apple_smc_read_s32(power->smc, SMC_KEY(B0AP), &vs32);
		val->intval = vs32 * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(BITV), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0RC), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = apple_smc_read_u32(power->smc, SMC_KEY(CSIL), &vu32);
		val->intval = vu32 * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0RI), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0RV), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0DC), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0FC), &vu16);
		val->intval = vu16 * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0RM), &vu16);
		val->intval = swab16(vu16) * 1000;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0AT), &vu16);
		val->intval = vu16 - 2732;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		ret = apple_smc_read_s64(power->smc, SMC_KEY(BAAC), &vs64);
		val->intval = vs64;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = apple_smc_read_u16(power->smc, SMC_KEY(B0CT), &vu16);
		val->intval = vu16;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = apple_smc_read_flag(power->smc, SMC_KEY(BBAD));
		val->intval = ret == 1 ? POWER_SUPPLY_HEALTH_DEAD : POWER_SUPPLY_HEALTH_GOOD;
		ret = ret < 0 ? ret : 0;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = power->model_name;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = power->serial_number;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static enum power_supply_property macsmc_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const struct power_supply_desc macsmc_battery_desc = {
	.name		= "macsmc-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.get_property	= macsmc_battery_get_property,
	.properties	= macsmc_battery_props,
	.num_properties	= ARRAY_SIZE(macsmc_battery_props),
};

static int macsmc_power_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct macsmc_power *power = container_of(nb, struct macsmc_power, nb);

	if ((event & 0xffffff00) == 0x71010100) {
		bool charging = (event & 0xff) != 0;

		dev_info(power->dev, "Charging: %d\n", charging);
		power_supply_changed(power->psy);

		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static int macsmc_power_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct macsmc_power *power;
	int ret;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;

	power->dev = &pdev->dev;
	power->smc = smc;
	dev_set_drvdata(&pdev->dev, power);

	/* Ignore devices without a charger/battery */
	if (macsmc_battery_get_status(power) <= POWER_SUPPLY_STATUS_UNKNOWN)
		return -ENODEV;

	/* Fetch string properties */
	apple_smc_read(smc, SMC_KEY(BMDN), power->model_name, sizeof(power->model_name) - 1);
	apple_smc_read(smc, SMC_KEY(BMSN), power->serial_number, sizeof(power->serial_number) - 1);

	psy_cfg.drv_data = power;
	power->psy = devm_power_supply_register(&pdev->dev, &macsmc_battery_desc, &psy_cfg);
	if (IS_ERR(power->psy)) {
		dev_err(&pdev->dev, "Failed to register power supply\n");
		ret = PTR_ERR(power->psy);
		return ret;
	}

	power->nb.notifier_call = macsmc_power_event;
	apple_smc_register_notifier(power->smc, &power->nb);

	return 0;
}

static int macsmc_power_remove(struct platform_device *pdev)
{
	struct macsmc_power *power = dev_get_drvdata(&pdev->dev);

	apple_smc_unregister_notifier(power->smc, &power->nb);

	return 0;
}

static struct platform_driver macsmc_power_driver = {
	.driver = {
		.name = "macsmc-power",
		.owner = THIS_MODULE,
	},
	.probe = macsmc_power_probe,
	.remove = macsmc_power_remove,
};
module_platform_driver(macsmc_power_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC battery and power management driver");
MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_ALIAS("platform:macsmc-power");
