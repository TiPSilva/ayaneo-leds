// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Joystick ring RGB LEDs of Ayaneo handhelds with the legacy EC interface.
 *
 * The rings are driven through the plain ACPI EC (ec_read/ec_write), the same
 * interface the in-tree ayaneo-ec driver uses for fan and charge control. This
 * driver only handles the LEDs, so it can sit next to ayaneo-ec. The register
 * protocol was documented by the out-of-tree ayaneo-platform driver.
 *
 * Three multicolor LEDs share the rings:
 *   ayaneo:rgb:joystick_rings        3 channels, both rings (kept for InputPlumber
 *                                    and other users of the original interface)
 *   ayaneo:rgb:joystick_ring_left    12 channels: 4 zones x red/green/blue
 *   ayaneo:rgb:joystick_ring_right   12 channels: 4 zones x red/green/blue
 * The last one written wins. Writing "1" to ec_control on the combined LED
 * hands the rings back to the EC animation until the next write.
 *
 * The EC can redraw the rings on its own (after the charger is plugged or
 * unplugged), and other tools may write it directly. The driver takes the
 * rings back after a charger change, and the first write after a pause
 * rewrites every channel instead of only the changed ones.
 *
 * Copyright (C) 2026 Tiago Silva
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/*
 * Each write selects a group (left ring, right ring, or both to commit), a
 * position and a value, then latches it with the write mode and returns to
 * hold mode, which keeps the rings under host control. Each ring has four
 * zones, red/green/blue at consecutive positions. Releasing hands the rings
 * back to the EC animation.
 */
#define AYANEO_LED_GROUP_REG		0x6d
#define AYANEO_LED_POS_REG		0xb1
#define AYANEO_LED_VAL_REG		0xb2
#define AYANEO_LED_MODE_REG		0xbf
#define AYANEO_LED_MODE_RELEASE		0x00
#define AYANEO_LED_MODE_WRITE		0x10
#define AYANEO_LED_MODE_HOLD		0xfe

#define AYANEO_LED_GROUP_LEFT		0x01
#define AYANEO_LED_GROUP_RIGHT		0x02
#define AYANEO_LED_GROUP_COMMIT		0x03

#define AYANEO_LED_POS_ENABLE		0x02
#define AYANEO_LED_ENABLE_ON		0xb1
#define AYANEO_LED_ENABLE_OFF		0x31
#define AYANEO_LED_ENABLE_RESET		0xc0

#define AYANEO_LED_WRITE_DELAY_MS	2
/* Time for the EC to finish its own reaction to a charger change. */
#define AYANEO_LED_AC_REFRESH_MS	2000
/* A write after this long without writes rewrites every channel. */
#define AYANEO_LED_IDLE_REFRESH_MS	2000

#define AYANEO_LED_SIDES		2
#define AYANEO_LED_ZONES		4

/*
 * Full-scale value per ring. Some models drive one ring brighter than the
 * other at the same value, so the OEM software caps each side differently.
 */
struct ayaneo_leds_model {
	u8 max_left;
	u8 max_right;
};

enum ayaneo_led_id {
	AYANEO_LED_BOTH,
	AYANEO_LED_LEFT,
	AYANEO_LED_RIGHT,
	AYANEO_LED_COUNT,
};

struct ayaneo_led {
	struct ayaneo_leds_data *data;
	enum ayaneo_led_id id;
	struct led_classdev_mc mc;
	struct mc_subled subled[AYANEO_LED_ZONES * 3];
};

struct ayaneo_leds_data {
	struct device *dev;
	const struct ayaneo_leds_model *model;
	struct ayaneo_led led[AYANEO_LED_COUNT];
	struct notifier_block psy_nb;
	struct delayed_work refresh;
	/* Protects the EC LED sequence and everything below */
	struct mutex lock;
	bool held;	/* the EC is in hold mode right now */
	bool active;	/* userspace set the rings; restore after resume */
	bool parked;	/* suspended or shutting down: only remember colours */
	unsigned long last_request;	/* jiffies of the last write */
	/* What userspace asked for, already scaled for the EC. */
	u8 want[AYANEO_LED_SIDES][AYANEO_LED_ZONES][3];
	/* What the EC has; only changed channels are written. */
	u8 hw[AYANEO_LED_SIDES][AYANEO_LED_ZONES][3];
	bool hw_valid;
};

/* Full scale drives the rings brighter than the OEM software does. */
static const struct ayaneo_leds_model model_default = {
	.max_left = 192,
	.max_right = 192,
};

/* The right ring is brighter; values from ayaneo-platform. */
static const struct ayaneo_leds_model model_air_1s_limited = {
	.max_left = 192,
	.max_right = 153,
};

static const struct ayaneo_leds_model model_air_plus_mendocino = {
	.max_left = 48,
	.max_right = 24,
};

#define AYANEO_LEDS_DMI(vendor, board, m)				\
	{								\
		.matches = {						\
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, vendor),	\
			DMI_EXACT_MATCH(DMI_BOARD_NAME, board),		\
		},							\
		.driver_data = (void *)&(m),				\
	}

/* Models confirmed on real hardware. These load automatically. */
static const struct dmi_system_id dmi_table[] = {
	AYANEO_LEDS_DMI("AYANEO", "AYANEO 2S", model_default),
	{},
};

/*
 * Models that use the same legacy EC interface according to ayaneo-platform
 * but have not been confirmed yet. They only load with untested=1. Once a
 * report confirms one, move its line to dmi_table.
 *
 * Not listed: the KUN (different zone layout plus a button LED) and the
 * AIR Plus AMD and Slide, which use a different EC interface.
 */
static const struct dmi_system_id dmi_table_untested[] = {
	AYANEO_LEDS_DMI("AYANEO", "AYANEO 2", model_default),
	AYANEO_LEDS_DMI("AYANEO", "GEEK", model_default),
	AYANEO_LEDS_DMI("AYANEO", "GEEK 1S", model_default),
	AYANEO_LEDS_DMI("AYANEO", "AIR", model_default),
	AYANEO_LEDS_DMI("AYANEO", "AIR Pro", model_default),
	AYANEO_LEDS_DMI("AYANEO", "AIR 1S", model_default),
	AYANEO_LEDS_DMI("AYANEO", "AIR 1S Limited", model_air_1s_limited),
	AYANEO_LEDS_DMI("AYANEO", "AB05-Mendocino", model_air_plus_mendocino),
	AYANEO_LEDS_DMI("Mysten Labs, Inc.", "SuiPlay0X1", model_default),
	{},
};

static bool untested;
module_param(untested, bool, 0444);
MODULE_PARM_DESC(untested,
		 "Also load on models that have not been confirmed yet (see TESTING.md)");

static int ayaneo_led_write(u8 group, u8 pos, u8 val)
{
	int ret;

	ret = ec_write(AYANEO_LED_GROUP_REG, group);
	if (ret)
		return ret;
	ret = ec_write(AYANEO_LED_POS_REG, pos);
	if (ret)
		return ret;
	ret = ec_write(AYANEO_LED_VAL_REG, val);
	if (ret)
		return ret;
	ret = ec_write(AYANEO_LED_MODE_REG, AYANEO_LED_MODE_WRITE);
	if (ret)
		return ret;

	msleep(AYANEO_LED_WRITE_DELAY_MS);

	return ec_write(AYANEO_LED_MODE_REG, AYANEO_LED_MODE_HOLD);
}

static int ayaneo_led_enable(u8 cmd)
{
	int ret;

	ret = ayaneo_led_write(AYANEO_LED_GROUP_LEFT, AYANEO_LED_POS_ENABLE, cmd);
	if (ret)
		return ret;
	ret = ayaneo_led_write(AYANEO_LED_GROUP_RIGHT, AYANEO_LED_POS_ENABLE, cmd);
	if (ret)
		return ret;

	return ayaneo_led_write(AYANEO_LED_GROUP_COMMIT, 0, 0);
}

static int ayaneo_led_hold(struct ayaneo_leds_data *data)
{
	int ret;

	if (data->held)
		return 0;

	ret = ec_write(AYANEO_LED_MODE_REG, AYANEO_LED_MODE_HOLD);
	if (ret)
		return ret;
	ret = ayaneo_led_enable(AYANEO_LED_ENABLE_RESET);
	if (ret)
		return ret;
	ret = ayaneo_led_enable(AYANEO_LED_ENABLE_OFF);
	if (ret)
		return ret;

	data->held = true;
	return 0;
}

/*
 * Reset before releasing: without it the EC keeps showing the last host
 * colour. The EC resumes its own indication on the next power event.
 */
static int ayaneo_led_release(struct ayaneo_leds_data *data)
{
	int ret;

	if (!data->held)
		return 0;

	ret = ayaneo_led_enable(AYANEO_LED_ENABLE_RESET);
	if (ret)
		return ret;
	ret = ec_write(AYANEO_LED_MODE_REG, AYANEO_LED_MODE_RELEASE);
	if (ret)
		return ret;

	data->held = false;
	return 0;
}

static const u8 ayaneo_led_group_regs[] = { AYANEO_LED_GROUP_LEFT, AYANEO_LED_GROUP_RIGHT };
static const u8 ayaneo_led_zones[] = { 3, 6, 9, 12 };

/*
 * Push `want` to the EC. Effects rewrite the rings several times per second,
 * so only the channels that changed are sent, and a ring is committed only if
 * something on it changed. A full frame of both rings is 24 channel writes.
 */
static int ayaneo_led_flush(struct ayaneo_leds_data *data)
{
	int g, z, i, ret;

	ret = ayaneo_led_hold(data);
	if (ret)
		return ret;
	if (!data->hw_valid) {
		ret = ayaneo_led_enable(AYANEO_LED_ENABLE_ON);
		if (ret)
			return ret;
	}

	for (g = 0; g < AYANEO_LED_SIDES; g++) {
		bool dirty = false;

		for (z = 0; z < AYANEO_LED_ZONES; z++) {
			for (i = 0; i < 3; i++) {
				u8 v = data->want[g][z][i];

				if (data->hw_valid && data->hw[g][z][i] == v)
					continue;
				ret = ayaneo_led_write(ayaneo_led_group_regs[g],
						       ayaneo_led_zones[z] + i, v);
				if (ret) {
					data->hw_valid = false;
					return ret;
				}
				data->hw[g][z][i] = v;
				dirty = true;
			}
		}
		if (dirty) {
			ret = ayaneo_led_write(AYANEO_LED_GROUP_COMMIT, 0, 0);
			if (ret) {
				data->hw_valid = false;
				return ret;
			}
		}
	}
	data->hw_valid = true;
	return 0;
}

static u8 ayaneo_led_scale(u32 v, u8 max, u32 max_brightness)
{
	u8 out = v * max / max_brightness;

	/* Keep dim channels lit so both rings match at low levels. */
	return (!out && v) ? 1 : out;
}

static int ayaneo_led_brightness_set(struct led_classdev *cdev,
				     enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct ayaneo_led *led = container_of(mc, struct ayaneo_led, mc);
	struct ayaneo_leds_data *data = led->data;
	const u8 max[] = { data->model->max_left, data->model->max_right };
	unsigned long now = jiffies;
	int g, z, i;

	guard(mutex)(&data->lock);
	cdev->brightness = brightness;
	led_mc_calc_color_components(mc, brightness);

	for (g = 0; g < AYANEO_LED_SIDES; g++) {
		if (led->id == AYANEO_LED_LEFT && g != 0)
			continue;
		if (led->id == AYANEO_LED_RIGHT && g != 1)
			continue;
		for (z = 0; z < AYANEO_LED_ZONES; z++) {
			for (i = 0; i < 3; i++) {
				/* The combined LED has one color for every zone. */
				int ch = led->id == AYANEO_LED_BOTH ? i : z * 3 + i;
				u32 v = mc->subled_info[ch].brightness;

				data->want[g][z][i] = ayaneo_led_scale(v, max[g],
								       cdev->max_brightness);
			}
		}
	}

	/*
	 * Effects write continuously and only need the changed channels. A
	 * write after a pause may follow something else drawing on the rings
	 * (another tool writing the EC, the EC itself), so resend everything.
	 */
	if (time_after(now, data->last_request +
		       msecs_to_jiffies(AYANEO_LED_IDLE_REFRESH_MS)))
		data->hw_valid = false;
	data->last_request = now;

	data->active = true;
	if (data->parked)
		return 0;
	return ayaneo_led_flush(data);
}

static void ayaneo_led_release_action(void *arg)
{
	struct ayaneo_leds_data *data = arg;

	guard(mutex)(&data->lock);
	ayaneo_led_release(data);
	data->hw_valid = false;
}

/*
 * The EC may redraw the rings after a charger change. Take them back once it
 * is done, the same way as after resume.
 */
static void ayaneo_leds_refresh_work(struct work_struct *work)
{
	struct ayaneo_leds_data *data = container_of(to_delayed_work(work),
						     struct ayaneo_leds_data, refresh);
	int ret;

	guard(mutex)(&data->lock);
	if (!data->active || data->parked)
		return;
	data->held = false;
	data->hw_valid = false;
	ret = ayaneo_led_flush(data);
	if (ret)
		dev_warn(data->dev, "could not restore the rings after a charger change: %d\n",
			 ret);
}

static int ayaneo_leds_psy_notify(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct ayaneo_leds_data *data = container_of(nb, struct ayaneo_leds_data, psy_nb);
	struct power_supply *psy = ptr;

	if (event != PSY_EVENT_PROP_CHANGED || psy->desc->type != POWER_SUPPLY_TYPE_MAINS)
		return NOTIFY_DONE;

	/* Restart the wait on every change, so a quick replug refreshes once. */
	cancel_delayed_work(&data->refresh);
	schedule_delayed_work(&data->refresh, msecs_to_jiffies(AYANEO_LED_AC_REFRESH_MS));
	return NOTIFY_OK;
}

static void ayaneo_leds_psy_unregister(void *arg)
{
	struct ayaneo_leds_data *data = arg;

	power_supply_unreg_notifier(&data->psy_nb);
	cancel_delayed_work_sync(&data->refresh);
}

static ssize_t ec_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct ayaneo_led *led = container_of(lcdev_to_mccdev(cdev), struct ayaneo_led, mc);

	guard(mutex)(&led->data->lock);
	return sysfs_emit(buf, "%d\n", !led->data->held);
}

/* Writing 1 hands the rings back to the EC animation until the next color. */
static ssize_t ec_control_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct ayaneo_led *led = container_of(lcdev_to_mccdev(cdev), struct ayaneo_led, mc);
	struct ayaneo_leds_data *data = led->data;
	bool release;
	int ret;

	ret = kstrtobool(buf, &release);
	if (ret)
		return ret;
	if (!release)
		return -EINVAL;

	guard(mutex)(&data->lock);
	ret = ayaneo_led_release(data);
	if (ret)
		return ret;
	data->active = false;
	data->hw_valid = false;
	return count;
}

static DEVICE_ATTR_RW(ec_control);

static struct attribute *ayaneo_led_attrs[] = {
	&dev_attr_ec_control.attr,
	NULL
};

static const struct attribute_group ayaneo_led_group = {
	.attrs = ayaneo_led_attrs,
};

static int ayaneo_led_register(struct device *dev, struct ayaneo_leds_data *data,
			       enum ayaneo_led_id id, const char *name)
{
	static const int colors[] = { LED_COLOR_ID_RED, LED_COLOR_ID_GREEN,
				      LED_COLOR_ID_BLUE };
	struct ayaneo_led *led = &data->led[id];
	struct led_classdev *cdev = &led->mc.led_cdev;
	int n = id == AYANEO_LED_BOTH ? 3 : AYANEO_LED_ZONES * 3;
	int i;

	led->data = data;
	led->id = id;
	for (i = 0; i < n; i++) {
		led->subled[i].color_index = colors[i % 3];
		led->subled[i].channel = i;
		led->subled[i].intensity = LED_FULL;
	}
	led->mc.subled_info = led->subled;
	led->mc.num_colors = n;

	cdev->name = name;
	cdev->color = LED_COLOR_ID_RGB;
	cdev->max_brightness = LED_FULL;
	/*
	 * Unregistering must not switch the rings off: the release action
	 * hands them back to the EC, and leaves them alone if they were never
	 * taken.
	 */
	cdev->flags = LED_RETAIN_AT_SHUTDOWN;
	/*
	 * The EC keeps running its own animation until the first write, so
	 * loading the driver does not change what the rings show.
	 */
	cdev->brightness = 0;
	cdev->brightness_set_blocking = ayaneo_led_brightness_set;

	return devm_led_classdev_multicolor_register(dev, &led->mc);
}

static int ayaneo_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct dmi_system_id *entry;
	struct ayaneo_leds_data *data;
	int ret;

	entry = dmi_first_match(dmi_table);
	if (!entry) {
		entry = dmi_first_match(dmi_table_untested);
		if (!entry)
			return -ENODEV;
		if (!untested) {
			dev_info(dev, "%s is not confirmed yet, load with untested=1 to test it\n",
				 entry->matches[1].substr);
			return -ENODEV;
		}
		dev_warn(dev, "%s is not confirmed yet, please report the result\n",
			 entry->matches[1].substr);
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;
	data->model = entry->driver_data;
	data->last_request = jiffies;

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, data);

	/* Registered first so it runs last: hand the rings back to the EC. */
	ret = devm_add_action_or_reset(dev, ayaneo_led_release_action, data);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&data->refresh, ayaneo_leds_refresh_work);
	data->psy_nb.notifier_call = ayaneo_leds_psy_notify;
	ret = power_supply_reg_notifier(&data->psy_nb);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, ayaneo_leds_psy_unregister, data);
	if (ret)
		return ret;

	ret = ayaneo_led_register(dev, data, AYANEO_LED_BOTH, "ayaneo:rgb:joystick_rings");
	if (ret)
		return ret;
	/*
	 * The multicolor core owns cdev->groups (multi_intensity, multi_index), so
	 * ec_control is added to the class device after registration.
	 */
	ret = devm_device_add_group(data->led[AYANEO_LED_BOTH].mc.led_cdev.dev,
				    &ayaneo_led_group);
	if (ret)
		return ret;
	ret = ayaneo_led_register(dev, data, AYANEO_LED_LEFT, "ayaneo:rgb:joystick_ring_left");
	if (ret)
		return ret;
	return ayaneo_led_register(dev, data, AYANEO_LED_RIGHT, "ayaneo:rgb:joystick_ring_right");
}

/*
 * Let the EC show its own sleep and charging indication. Writes that arrive
 * while parked (a trigger, a queued sysfs write) only update the colours
 * restored on resume. An EC error must not abort the system suspend.
 */
static int ayaneo_leds_suspend(struct device *dev)
{
	struct ayaneo_leds_data *data = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&data->lock);
	data->parked = true;
	data->hw_valid = false;
	ret = ayaneo_led_release(data);
	if (ret)
		dev_warn(dev, "could not hand the rings back to the EC: %d\n", ret);
	return 0;
}

static int ayaneo_leds_resume(struct device *dev)
{
	struct ayaneo_leds_data *data = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&data->lock);
	data->parked = false;
	if (!data->active)
		return 0;
	ret = ayaneo_led_flush(data);
	if (ret)
		dev_warn(dev, "could not restore the rings: %d\n", ret);
	return 0;
}

static void ayaneo_leds_shutdown(struct platform_device *pdev)
{
	struct ayaneo_leds_data *data = platform_get_drvdata(pdev);

	if (!data)
		return;

	/* Stay with the EC even if something writes after this point. */
	guard(mutex)(&data->lock);
	data->parked = true;
	ayaneo_led_release(data);
	data->hw_valid = false;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ayaneo_leds_pm_ops, ayaneo_leds_suspend,
				ayaneo_leds_resume);

static struct platform_driver ayaneo_leds_driver = {
	.driver = {
		.name = "ayaneo-leds",
		.pm = pm_sleep_ptr(&ayaneo_leds_pm_ops),
	},
	.probe = ayaneo_leds_probe,
	.shutdown = ayaneo_leds_shutdown,
};

static struct platform_device *ayaneo_leds_device;

static int __init ayaneo_leds_init(void)
{
	ayaneo_leds_device =
		platform_create_bundle(&ayaneo_leds_driver,
				       ayaneo_leds_probe, NULL, 0, NULL, 0);

	return PTR_ERR_OR_ZERO(ayaneo_leds_device);
}

static void __exit ayaneo_leds_exit(void)
{
	platform_device_unregister(ayaneo_leds_device);
	platform_driver_unregister(&ayaneo_leds_driver);
}

MODULE_DEVICE_TABLE(dmi, dmi_table);

module_init(ayaneo_leds_init);
module_exit(ayaneo_leds_exit);

MODULE_VERSION("0.2.1");
MODULE_AUTHOR("Tiago Silva");
MODULE_DESCRIPTION("Ayaneo joystick ring RGB LEDs (legacy EC)");
MODULE_LICENSE("GPL");
