/*
 *  Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/leds.h>
#include <linux/workqueue.h>

#include "inc/mt6370_pmu.h"
#include "inc/mt6370_pmu_rgbled.h"

enum {
	MT6370_PMU_LED_PWMMODE = 0,
	MT6370_PMU_LED_BREATHMODE,
	MT6370_PMU_LED_REGMODE,
	MT6370_PMU_LED_MAXMODE,
};

struct mt6370_led_classdev {
	struct led_classdev led_dev;
	int led_index;
};

struct mt6370_pmu_rgbled_data {
	struct mt6370_pmu_chip *chip;
	struct device *dev;
	struct delayed_work dwork;
};

#define MT_LED_ATTR(_name) {\
	.attr = {\
		.name = #_name,\
		.mode = 0644,\
	},\
	.show = mt_led_##_name##_attr_show,\
	.store = mt_led_##_name##_attr_store,\
}

static const uint8_t rgbled_init_data[] = {
	0x60, /* MT6370_PMU_REG_RGB1DIM: 0x82 */
	0x60, /* MT6370_PMU_REG_RGB2DIM: 0x83 */
	0x60, /* MT6370_PMU_REG_RGB3DIM: 0x84 */
	0x0f, /* MT6370_PMU_REG_RGBEN: 0x85 */
	0x08, /* MT6370_PMU_REG_RGB1ISINK: 0x86 */
	0x08, /* MT6370_PMU_REG_RGB2ISINK: 0x87 */
	0x08, /* MT6370_PMU_REG_RGB3ISINK: 0x88 */
	0x52, /* MT6370_PMU_REG_RGB1TR: 0x89 */
	0x25, /* MT6370_PMU_REG_RGB1TF: 0x8A */
	0x11, /* MT6370_PMU_REG_RGB1TONTOFF: 0x8B */
	0x52, /* MT6370_PMU_REG_RGB2TR: 0x8C */
	0x25, /* MT6370_PMU_REG_RGB2TF: 0x8D */
	0x11, /* MT6370_PMU_REG_RGB2TONTOFF: 0x8E */
	0x52, /* MT6370_PMU_REG_RGB3TR: 0x8F */
	0x25, /* MT6370_PMU_REG_RGB3TF: 0x90 */
	0x11, /* MT6370_PMU_REG_RGB3TONTOFF: 0x91 */
	0x60, /* MT6370_PMU_REG_RGBCHRINDDIM: 0x92 */
	0x07, /* MT6370_PMU_REG_RGBCHRINDCTRL: 0x93 */
	0x52, /* MT6370_PMU_REG_RGBCHRINDTR: 0x94 */
	0x25, /* MT6370_PMU_REG_RGBCHRINDTF: 0x95 */
	0x11, /* MT6370_PMU_REG_RGBCHRINDTONTOFF: 0x96 */
	0xff, /* MT6370_PMU_REG_RGBOPENSHORTEN: 0x97 */
};

static inline int mt6370_pmu_led_get_index(struct led_classdev *led_cdev)
{
	struct mt6370_led_classdev *mt_led_cdev =
				(struct mt6370_led_classdev *)led_cdev;

	return mt_led_cdev->led_index;
}

static inline int mt6370_pmu_led_update_bits(struct led_classdev *led_cdev,
	uint8_t reg_addr, uint8_t reg_mask, uint8_t reg_data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data =
				dev_get_drvdata(led_cdev->dev->parent);

	return mt6370_pmu_reg_update_bits(rgbled_data->chip,
					reg_addr, reg_mask, reg_data);
}

static inline int mt6370_pmu_led_reg_read(struct led_classdev *led_cdev,
	uint8_t reg_addr)
{
	struct mt6370_pmu_rgbled_data *rgbled_data =
				dev_get_drvdata(led_cdev->dev->parent);

	return mt6370_pmu_reg_read(rgbled_data->chip, reg_addr);
}

static inline void mt6370_pmu_led_enable_dwork(struct led_classdev *led_cdev)
{
	struct mt6370_pmu_rgbled_data *rgbled_data =
				dev_get_drvdata(led_cdev->dev->parent);

	cancel_delayed_work_sync(&rgbled_data->dwork);
	schedule_delayed_work(&rgbled_data->dwork, msecs_to_jiffies(100));
}

static void mt6370_pmu_led_bright_set(struct led_classdev *led_cdev,
	enum led_brightness bright)
{
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0, reg_mask = 0x7, reg_shift = 0, en_mask = 0;
	bool need_enable_timer = true;
	int ret = 0;

	dev_err(led_cdev->dev, "mt6370_pmu_led_bright_set, led_index=%d, brightness = %d\n", led_index, bright);
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		en_mask = 0x80;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		en_mask = 0x40;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		en_mask = 0x20;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x3;
		en_mask = 0x10;
		need_enable_timer = false;
		break;
	default:
		dev_err(led_cdev->dev, "invalid mt led index\n");
		return;
	}
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr, reg_mask,
					 (bright & reg_mask) << reg_shift);
	if (ret < 0) {
		dev_err(led_cdev->dev, "update brightness fail\n");
		return;
	}
	if (!bright)
		need_enable_timer = false;
	if (need_enable_timer) {
		mt6370_pmu_led_enable_dwork(led_cdev);
		return;
	}
	ret = mt6370_pmu_led_update_bits(led_cdev, MT6370_PMU_REG_RGBEN,
					 en_mask,
					 (bright > 0) ? en_mask : ~en_mask);
	if (ret < 0)
		dev_err(led_cdev->dev, "update enable bit fail\n");
}

static enum led_brightness mt6370_pmu_led_bright_get(
	struct led_classdev *led_cdev)
{
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0, reg_mask = 0x7, reg_shift = 0, en_mask = 0;
	bool need_enable_timer = true;
	int ret = 0;

	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		en_mask = 0x80;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		en_mask = 0x40;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		en_mask = 0x20;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x3;
		en_mask = 0x10;
		need_enable_timer = false;
		break;
	default:
		dev_err(led_cdev->dev, "invalid mt led index\n");
		return -EINVAL;
	}
	ret = mt6370_pmu_led_reg_read(led_cdev, MT6370_PMU_REG_RGBEN);
	if (ret < 0)
		return ret;
	if (!(ret & en_mask))
		return LED_OFF;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	return ((uint8_t)ret & reg_mask) >> reg_shift;
}

static inline int mt6370_pmu_led_config_pwm(struct led_classdev *led_cdev,
	unsigned long *delay_on, unsigned long *delay_off)
{
	const ulong dim_time[] = { 10000, 5000, 2000, 1000, 500, 200, 5, 1};
	const unsigned long ton = *delay_on, toff = *delay_off;
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr, reg_mask, reg_shift, j;
	int ret = 0, i = 0;

	dev_dbg(led_cdev->dev, "%s, on %lu, off %lu\n", __func__, ton, toff);
	/* find the close dim freq */
	for (i = ARRAY_SIZE(dim_time) - 1; i >= 0; i--) {
		if (dim_time[i] >= (ton + toff))
			break;
	}
	if (i < 0) {
		dev_warn(led_cdev->dev, "no match, sum %lu\n", ton + toff);
		i = 0;
	}
	/* write pwm dim freq selection */
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		reg_mask = 0x38;
		reg_shift = 3;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		reg_mask = 0x38;
		reg_shift = 3;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		reg_mask = 0x38;
		reg_shift = 3;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x1c;
		reg_shift = 2;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret < 0)
		return ret;

	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 reg_mask, i << reg_shift);
	if (ret < 0)
		return ret;
	/* find the closest pwm duty */
	j = 32 * ton / (ton + toff);
	if (j == 0)
		j = 1;
	j--;
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1DIM;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2DIM;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3DIM;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDDIM;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret < 0)
		return ret;

	reg_mask = MT6370_LED_PWMDUTYMASK;
	reg_shift = MT6370_LED_PWMDUTYSHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 reg_mask, j << reg_shift);
	if (ret < 0)
		return ret;
	return 0;
}

static int mt6370_pmu_led_change_mode(struct led_classdev *led_cdev,
						uint8_t mode);
static int mt6370_pmu_led_blink_set(struct led_classdev *led_cdev,
	unsigned long *delay_on, unsigned long *delay_off)
{
	uint8_t mode_sel = MT6370_PMU_LED_PWMMODE;
	int ret = 0;

	dev_warn(led_cdev->dev, "%s: on=%lu, off=%lu\n", __func__, *delay_on, *delay_off);
	if (!*delay_on && !*delay_off)
		*delay_on = *delay_off = 500;
	if (!*delay_on) {
		led_set_brightness(led_cdev, LED_OFF);
		return 0;
	}
	if (!*delay_off)
		mode_sel = MT6370_PMU_LED_REGMODE;
	if (mode_sel == MT6370_PMU_LED_PWMMODE) {
		/* workaround, fix cc to pwm */
		ret = mt6370_pmu_led_change_mode(led_cdev,
						 MT6370_PMU_LED_BREATHMODE);
		if (ret < 0)
			dev_err(led_cdev->dev, "%s: mode fix fail\n", __func__);
		ret = mt6370_pmu_led_config_pwm(led_cdev, delay_on, delay_off);
		if (ret < 0)
			dev_err(led_cdev->dev, "%s: cfg pwm fail\n", __func__);
	}
	ret = mt6370_pmu_led_change_mode(led_cdev, mode_sel);
	if (ret < 0)
		dev_err(led_cdev->dev, "%s: change mode fail\n", __func__);
	return 0;
}

static struct mt6370_led_classdev mt6370_led_classdev[MT6370_PMU_MAXLED] = {
	{
		.led_dev =  {
			.max_brightness = 6,
			.brightness_set = mt6370_pmu_led_bright_set,
			.brightness_get = mt6370_pmu_led_bright_get,
			.blink_set = mt6370_pmu_led_blink_set,
		},
		.led_index = MT6370_PMU_LED1,
	},
	{
		.led_dev =  {
			.max_brightness = 6,
			.brightness_set = mt6370_pmu_led_bright_set,
			.brightness_get = mt6370_pmu_led_bright_get,
			.blink_set = mt6370_pmu_led_blink_set,
		},
		.led_index = MT6370_PMU_LED2,
	},
	{
		.led_dev =  {
			.max_brightness = 6,
			.brightness_set = mt6370_pmu_led_bright_set,
			.brightness_get = mt6370_pmu_led_bright_get,
			.blink_set = mt6370_pmu_led_blink_set,
		},
		.led_index = MT6370_PMU_LED3,
	},
	{
		.led_dev =  {
			.max_brightness = 3,
			.brightness_set = mt6370_pmu_led_bright_set,
			.brightness_get = mt6370_pmu_led_bright_get,
			.blink_set = mt6370_pmu_led_blink_set,
		},
		.led_index = MT6370_PMU_LED4,
	},
};

static int mt6370_pmu_led_change_mode(struct led_classdev *led_cdev,
					uint8_t mode)
{
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0;
	int ret = 0;

	if (mode >= MT6370_PMU_LED_MAXMODE)
		return -EINVAL;
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1DIM;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2DIM;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3DIM;
		break;
	case MT6370_PMU_LED4:
		/* disable auto mode */
		ret = mt6370_pmu_led_update_bits(led_cdev,
						 MT6370_PMU_REG_RGBCHRINDDIM,
						 0x80, 0x80);
		if (ret < 0)
			return ret;
		reg_addr = MT6370_PMU_REG_RGBCHRINDDIM;
		break;
	default:
		return -EINVAL;
	}
	return mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					  MT6370_LED_MODEMASK,
					  mode << MT6370_LED_MODESHFT);
}

static const char * const soft_start_str[] = {
	"0.5us",
	"1us",
	"1.5us",
	"2us",
};

static ssize_t mt_led_soft_start_step_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0, reg_mask = 0xc0, reg_shift = 6, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int i = 0, ret = 0;

	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x60;
		reg_shift = 5;
		break;
	default:
		return -EINVAL;
	}

	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = ((uint8_t)ret & reg_mask) >> reg_shift;
	ret = 0;
	for (i = 0; i < ARRAY_SIZE(soft_start_str); i++) {
		if (reg_data == i)
			ret += snprintf(buf + ret, cnt - ret, ">");
		ret += snprintf(buf + ret, cnt - ret, "%s ", soft_start_str[i]);
	}
	ret += snprintf(buf + ret, cnt - ret, "\n");
	return ret;
}

static ssize_t mt_led_soft_start_step_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0, reg_mask = 0xc0, reg_shift = 6, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store >= ARRAY_SIZE(soft_start_str))
		return -EINVAL;
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x60;
		reg_shift = 5;
		break;
	default:
		return -EINVAL;
	}
	reg_data = store << reg_shift;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 reg_mask, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static const struct device_attribute mt_led_cc_mode_attrs[] = {
	MT_LED_ATTR(soft_start_step),
};

static void mt6370_pmu_led_cc_activate(struct led_classdev *led_cdev)
{
	int i = 0, ret = 0;

	for (i = 0; i < ARRAY_SIZE(mt_led_cc_mode_attrs); i++) {
		ret = device_create_file(led_cdev->dev,
					 mt_led_cc_mode_attrs + i);
		if (ret < 0) {
			dev_err(led_cdev->dev,
				"%s: create file fail %d\n", __func__, i);
			goto out_create_file;
		}
	}
	ret = mt6370_pmu_led_change_mode(led_cdev, MT6370_PMU_LED_REGMODE);
	if (ret < 0) {
		dev_err(led_cdev->dev, "%s: change mode fail\n", __func__);
		goto out_change_mode;
	}
	return;
out_change_mode:
	i = ARRAY_SIZE(mt_led_cc_mode_attrs);
out_create_file:
	while (--i >= 0)
		device_remove_file(led_cdev->dev, mt_led_cc_mode_attrs + i);
}

static void mt6370_pmu_led_cc_deactivate(struct led_classdev *led_cdev)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(mt_led_cc_mode_attrs); i++)
		device_remove_file(led_cdev->dev, mt_led_cc_mode_attrs + i);
}

static ssize_t mt_led_pwm_duty_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0;
	int ret = 0;

	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1DIM;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2DIM;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3DIM;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDDIM;
		break;
	default:
		return -EINVAL;
	}
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_addr = ret & MT6370_LED_PWMDUTYMASK;
	return snprintf(buf, PAGE_SIZE, "%d (max: %d)\n", reg_addr,
			MT6370_LED_PWMDUTYMAX);
}

static ssize_t mt_led_pwm_duty_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LED_PWMDUTYMAX)
		return -EINVAL;
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1DIM;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2DIM;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3DIM;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDDIM;
		break;
	default:
		return -EINVAL;
	}
	reg_data = store << MT6370_LED_PWMDUTYSHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LED_PWMDUTYMASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static const char * const led_dim_freq[] = {
	"0.1Hz",
	"0.2Hz",
	"0.5Hz",
	"1Hz",
	"2Hz",
	"5Hz",
	"200Hz",
	"1KHz",
};

static ssize_t mt_led_pwm_dim_freq_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0,  reg_mask = 0x38, reg_shift = 3, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int i = 0, ret = 0;

	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x1c;
		reg_shift = 2;
		break;
	default:
		return -EINVAL;
	}
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = ((uint8_t)ret & reg_mask) >> reg_shift;
	ret = 0;
	for (i = 0; i < ARRAY_SIZE(led_dim_freq); i++) {
		if (reg_data == i)
			ret += snprintf(buf + ret, cnt - ret, ">");
		ret += snprintf(buf + ret, cnt - ret, "%s ", led_dim_freq[i]);
	}
	ret += snprintf(buf + ret, cnt - ret, "\n");
	return ret;
}

static ssize_t mt_led_pwm_dim_freq_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	uint8_t reg_addr = 0, reg_mask = 0x38, reg_shift = 3, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store >= ARRAY_SIZE(led_dim_freq))
		return -EINVAL;
	switch (led_index) {
	case MT6370_PMU_LED1:
		reg_addr = MT6370_PMU_REG_RGB1ISINK;
		break;
	case MT6370_PMU_LED2:
		reg_addr = MT6370_PMU_REG_RGB2ISINK;
		break;
	case MT6370_PMU_LED3:
		reg_addr = MT6370_PMU_REG_RGB3ISINK;
		break;
	case MT6370_PMU_LED4:
		reg_addr = MT6370_PMU_REG_RGBCHRINDCTRL;
		reg_mask = 0x1c;
		reg_shift = 2;
		break;
	default:
		return -EINVAL;
	}
	reg_data = store << reg_shift;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 reg_mask, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static const struct device_attribute mt_led_pwm_mode_attrs[] = {
	MT_LED_ATTR(pwm_duty),
	MT_LED_ATTR(pwm_dim_freq),
};
static void mt6370_pmu_led_pwm_activate(struct led_classdev *led_cdev)
{
	int i = 0, ret = 0;

	for (i = 0; i < ARRAY_SIZE(mt_led_pwm_mode_attrs); i++) {
		ret = device_create_file(led_cdev->dev,
					 mt_led_pwm_mode_attrs + i);
		if (ret < 0) {
			dev_err(led_cdev->dev,
				"%s: create file fail %d\n", __func__, i);
			goto out_create_file;
		}
	}

	/* workaround, fix cc to pwm */
	ret = mt6370_pmu_led_change_mode(led_cdev, MT6370_PMU_LED_BREATHMODE);
	if (ret < 0) {
		dev_err(led_cdev->dev, "%s: mode fix fail\n", __func__);
		goto out_change_mode;
	}
	ret = mt6370_pmu_led_change_mode(led_cdev, MT6370_PMU_LED_PWMMODE);
	if (ret < 0) {
		dev_err(led_cdev->dev, "%s: change mode fail\n", __func__);
		goto out_change_mode;
	}
	return;
out_change_mode:
	i = ARRAY_SIZE(mt_led_pwm_mode_attrs);
out_create_file:
	while (--i >= 0)
		device_remove_file(led_cdev->dev, mt_led_pwm_mode_attrs + i);
}

static void mt6370_pmu_led_pwm_deactivate(struct led_classdev *led_cdev)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(mt_led_pwm_mode_attrs); i++)
		device_remove_file(led_cdev->dev, mt_led_pwm_mode_attrs + i);
}

static int mt6370_pmu_led_get_breath_regbase(struct led_classdev *led_cdev)
{
	int led_index = mt6370_pmu_led_get_index(led_cdev);
	int ret = 0;

	switch (led_index) {
	case MT6370_PMU_LED1:
		ret = MT6370_PMU_REG_RGB1TR;
		break;
	case MT6370_PMU_LED2:
		ret = MT6370_PMU_REG_RGB2TR;
		break;
	case MT6370_PMU_LED3:
		ret = MT6370_PMU_REG_RGB3TR;
		break;
	case MT6370_PMU_LED4:
		ret = MT6370_PMU_REG_RGBCHRINDTR;
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static ssize_t mt_led_tr1_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 0, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int ret = 0;

	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = (ret & MT6370_LEDTR1_MASK) >> MT6370_LEDTR1_SHFT;
	return snprintf(buf, cnt, "%d (max 15, 0.125s, step 0.2s)\n", reg_data);
}

static ssize_t mt_led_tr1_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 0, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LEDBREATH_MAX)
		return -EINVAL;
	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	reg_data = store << MT6370_LEDTR1_SHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LEDTR1_MASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static ssize_t mt_led_tr2_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 0, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int ret = 0;

	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = (ret & MT6370_LEDTR2_MASK) >> MT6370_LEDTR2_SHFT;
	return snprintf(buf, cnt, "%d (max 15, 0.125s, step 0.2s)\n", reg_data);
}

static ssize_t mt_led_tr2_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 0, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LEDBREATH_MAX)
		return -EINVAL;
	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	reg_data = store << MT6370_LEDTR2_SHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LEDTR2_MASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static ssize_t mt_led_tf1_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 1, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int ret = 0;

	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = (ret & MT6370_LEDTF1_MASK) >> MT6370_LEDTF1_SHFT;
	return snprintf(buf, cnt, "%d (max 15, 0.125s, step 0.2s)\n", reg_data);
}

static ssize_t mt_led_tf1_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 1, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LEDBREATH_MAX)
		return -EINVAL;
	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	reg_data = store << MT6370_LEDTF1_SHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LEDTF1_MASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static ssize_t mt_led_tf2_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 1, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int ret = 0;

	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = (ret & MT6370_LEDTF2_MASK) >> MT6370_LEDTF2_SHFT;
	return snprintf(buf, cnt, "%d (max 15, 0.125s, step 0.2s)\n", reg_data);
}

static ssize_t mt_led_tf2_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 1, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LEDBREATH_MAX)
		return -EINVAL;
	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	reg_data = store << MT6370_LEDTF2_SHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LEDTF2_MASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static ssize_t mt_led_ton_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 2, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int ret = 0;

	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = (ret & MT6370_LEDTON_MASK) >> MT6370_LEDTON_SHFT;
	return snprintf(buf, cnt, "%d (max 15, 0.125s, step 0.2s)\n", reg_data);
}

static ssize_t mt_led_ton_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 2, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LEDBREATH_MAX)
		return -EINVAL;
	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	reg_data = store << MT6370_LEDTON_SHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LEDTON_MASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static ssize_t mt_led_toff_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 2, reg_data = 0;
	unsigned long cnt = PAGE_SIZE;
	int ret = 0;

	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	ret = mt6370_pmu_led_reg_read(led_cdev, reg_addr);
	if (ret < 0)
		return ret;
	reg_data = (ret & MT6370_LEDTOFF_MASK) >> MT6370_LEDTOFF_SHFT;
	return snprintf(buf, cnt, "%d (max 15, 0.125s, step 0.2s)\n", reg_data);
}

static ssize_t mt_led_toff_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	uint8_t reg_addr = 2, reg_data = 0;
	unsigned long store = 0;
	int ret = 0;

	ret = kstrtoul(buf, 10, &store);
	if (ret < 0)
		return ret;
	if (store > MT6370_LEDBREATH_MAX)
		return -EINVAL;
	ret = mt6370_pmu_led_get_breath_regbase(led_cdev);
	if (ret < 0)
		return ret;
	reg_addr += ret;
	reg_data = store << MT6370_LEDTOFF_SHFT;
	ret = mt6370_pmu_led_update_bits(led_cdev, reg_addr,
					 MT6370_LEDTOFF_MASK, reg_data);
	if (ret < 0)
		return ret;
	return cnt;
}

static const struct device_attribute mt_led_breath_mode_attrs[] = {
	MT_LED_ATTR(tr1),
	MT_LED_ATTR(tr2),
	MT_LED_ATTR(tf1),
	MT_LED_ATTR(tf2),
	MT_LED_ATTR(ton),
	MT_LED_ATTR(toff),
};

static void mt6370_pmu_led_breath_activate(struct led_classdev *led_cdev)
{
	int i = 0, ret = 0;

	for (i = 0; i < ARRAY_SIZE(mt_led_breath_mode_attrs); i++) {
		ret = device_create_file(led_cdev->dev,
				mt_led_breath_mode_attrs + i);
		if (ret < 0) {
			dev_err(led_cdev->dev,
				"%s: create file fail %d\n", __func__, i);
			goto out_create_file;
		}
	}

	ret = mt6370_pmu_led_change_mode(led_cdev, MT6370_PMU_LED_BREATHMODE);
	if (ret < 0) {
		dev_err(led_cdev->dev, "%s: change mode fail\n", __func__);
		goto out_change_mode;
	}
	return;
out_change_mode:
	i = ARRAY_SIZE(mt_led_breath_mode_attrs);
out_create_file:
	while (--i >= 0)
		device_remove_file(led_cdev->dev, mt_led_breath_mode_attrs + i);
}

static void mt6370_pmu_led_breath_deactivate(struct led_classdev *led_cdev)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(mt_led_breath_mode_attrs); i++)
		device_remove_file(led_cdev->dev, mt_led_breath_mode_attrs + i);
}

static struct led_trigger mt6370_pmu_led_trigger[] = {
	{
		.name = "cc_mode",
		.activate = mt6370_pmu_led_cc_activate,
		.deactivate = mt6370_pmu_led_cc_deactivate,
	},
	{
		.name = "pwm_mode",
		.activate = mt6370_pmu_led_pwm_activate,
		.deactivate = mt6370_pmu_led_pwm_deactivate,
	},
	{
		.name = "breath_mode",
		.activate = mt6370_pmu_led_breath_activate,
		.deactivate = mt6370_pmu_led_breath_deactivate,
	},
};

static irqreturn_t mt6370_pmu_isink4_short_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink3_short_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink2_short_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink1_short_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink4_open_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink3_open_irq_handler(int irq, void *data)

{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink2_open_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6370_pmu_isink1_open_irq_handler(int irq, void *data)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = data;

	dev_notice(rgbled_data->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static struct mt6370_pmu_irq_desc mt6370_rgbled_irq_desc[] = {
	MT6370_PMU_IRQDESC(isink4_short),
	MT6370_PMU_IRQDESC(isink3_short),
	MT6370_PMU_IRQDESC(isink2_short),
	MT6370_PMU_IRQDESC(isink1_short),
	MT6370_PMU_IRQDESC(isink4_open),
	MT6370_PMU_IRQDESC(isink3_open),
	MT6370_PMU_IRQDESC(isink2_open),
	MT6370_PMU_IRQDESC(isink1_open),
};

static void mt6370_pmu_rgbled_irq_register(struct platform_device *pdev)
{
	struct resource *res;
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(mt6370_rgbled_irq_desc); i++) {
		if (!mt6370_rgbled_irq_desc[i].name)
			continue;
		res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						mt6370_rgbled_irq_desc[i].name);
		if (!res)
			continue;
		ret = devm_request_threaded_irq(&pdev->dev, res->start, NULL,
					mt6370_rgbled_irq_desc[i].irq_handler,
					IRQF_TRIGGER_FALLING,
					mt6370_rgbled_irq_desc[i].name,
					platform_get_drvdata(pdev));
		if (ret < 0) {
			dev_err(&pdev->dev, "request %s irq fail\n", res->name);
			continue;
		}
		mt6370_rgbled_irq_desc[i].irq = res->start;
	}
}

static void mt6370_led_enable_dwork_func(struct work_struct *work)
{
	struct mt6370_pmu_rgbled_data *rgbled_data =
		container_of(work, struct mt6370_pmu_rgbled_data, dwork.work);
	uint8_t reg_data = 0, reg_mask = 0xe0;
	int ret = 0;

	dev_dbg(rgbled_data->dev, "%s\n", __func__);
	/* red */
	if (mt6370_led_classdev[0].led_dev.brightness != 0)
		reg_data |= 0x80;
	/* blue */
	if (mt6370_led_classdev[1].led_dev.brightness != 0)
		reg_data |= 0x40;
	/* green */
	if (mt6370_led_classdev[2].led_dev.brightness != 0)
		reg_data |= 0x20;
	ret =  mt6370_pmu_reg_update_bits(rgbled_data->chip,
					  MT6370_PMU_REG_RGBEN,
					  reg_mask, reg_data);
	if (ret < 0)
		dev_err(rgbled_data->dev, "timer update enable bit fail\n");
}

static inline int mt6370_pmu_rgbled_init_register(
	struct mt6370_pmu_rgbled_data *rgbled_data)
{
	return mt6370_pmu_reg_block_write(rgbled_data->chip,
					  MT6370_PMU_REG_RGB1DIM,
					  ARRAY_SIZE(rgbled_init_data),
					  rgbled_init_data);
}

static inline int mt6370_pmu_rgbled_parse_initdata(
	struct mt6370_pmu_rgbled_data *rgbled_data)
{
	return 0;
}

static inline int mt_parse_dt(struct device *dev)
{
	struct mt6370_pmu_rgbled_platdata *pdata = dev_get_platdata(dev);
	int name_cnt = 0, trigger_cnt = 0;
	struct device_node *np = dev->of_node;
	int ret = 0;

	while (true) {
		const char *name = NULL;

		ret = of_property_read_string_index(np, "mt,led_name",
						    name_cnt, &name);
		if (ret < 0)
			break;
		pdata->led_name[name_cnt] = name;
		name_cnt++;
	}
	while (true) {
		const char *name = NULL;

		ret = of_property_read_string_index(np,
						    "mt,led_default_trigger",
						    trigger_cnt, &name);
		if (ret < 0)
			break;
		pdata->led_default_trigger[trigger_cnt] = name;
		trigger_cnt++;
	}
	if (name_cnt != MT6370_PMU_MAXLED || trigger_cnt != MT6370_PMU_MAXLED)
		return -EINVAL;
	return 0;
}

static int mt6370_pmu_parse_brightness_dtsi(void)
{
	struct device_node* led_node = NULL;
	int ret = 0;
	unsigned int rled_max_brightness = 0;
	unsigned int gled_max_brightness = 0;
	unsigned int bled_max_brightness = 0;

	led_node = of_find_compatible_node(NULL, NULL, "mediatek,red");
	if (!led_node)
	{
		pr_err("Cannot find mediatek red from dts\n");
		return -1;
	}
	else
	{
		ret = of_property_read_u32(led_node, "max_brightness", &rled_max_brightness);
		if (!ret)
		{
			/* red */
			mt6370_led_classdev[0].led_dev.max_brightness = rled_max_brightness;
		}
		else
		{
			pr_err("Cannot find max_brightness red from dts\n");
			return -1;
		}
	}

	led_node = of_find_compatible_node(NULL, NULL, "mediatek,blue");
	if (!led_node)
	{
		pr_err("Cannot find mediatek blue from dts\n");
		return -1;
	}
	else
	{
		ret = of_property_read_u32(led_node, "max_brightness", &bled_max_brightness);
		if (!ret)
		{
			/* blue */
			mt6370_led_classdev[1].led_dev.max_brightness = bled_max_brightness;
		}
		else
		{
			pr_err("Cannot find max_brightness blue from dts\n");
			return -1;
		}
	}

	led_node = of_find_compatible_node(NULL, NULL, "mediatek,green");
	if (!led_node)
	{
		pr_err("Cannot find mediatek green from dts\n");
		return -1;
	}
	else
	{
		ret = of_property_read_u32(led_node, "max_brightness", &gled_max_brightness);
		if (!ret)
		{
			/* green */
			mt6370_led_classdev[2].led_dev.max_brightness = gled_max_brightness;
		}
		else
		{
			pr_err("Cannot find max_brightness green from dts\n");
			return -1;
		}
	}
	return 0;
}

static int mt6370_pmu_rgbled_probe(struct platform_device *pdev)
{
	struct mt6370_pmu_rgbled_platdata *pdata =
					dev_get_platdata(&pdev->dev);
	struct mt6370_pmu_rgbled_data *rgbled_data;
	bool use_dt = pdev->dev.of_node;
	int i = 0, ret = 0;

	rgbled_data = devm_kzalloc(&pdev->dev,
				   sizeof(*rgbled_data), GFP_KERNEL);
	if (!rgbled_data)
		return -ENOMEM;
	if (use_dt) {
		/* DTS used */
		pdata = devm_kzalloc(&pdev->dev,
				     sizeof(*rgbled_data), GFP_KERNEL);
		if (!pdata) {
			ret = -ENOMEM;
			goto out_pdata;
		}
		pdev->dev.platform_data = pdata;
		ret = mt_parse_dt(&pdev->dev);
		if (ret < 0) {
			devm_kfree(&pdev->dev, pdata);
			goto out_pdata;
		}
	} else {
		if (!pdata) {
			ret = -EINVAL;
			goto out_pdata;
		}
	}
	ret = mt6370_pmu_parse_brightness_dtsi();
	if (ret < 0)
	{
		dev_err(&pdev->dev, "parse max brightness dtsi fail\n");
	}
	rgbled_data->chip = dev_get_drvdata(pdev->dev.parent);
	rgbled_data->dev = &pdev->dev;
	platform_set_drvdata(pdev, rgbled_data);
	INIT_DELAYED_WORK(&rgbled_data->dwork, mt6370_led_enable_dwork_func);

	ret = mt6370_pmu_rgbled_parse_initdata(rgbled_data);
	if (ret < 0)
		goto out_init_data;

	ret = mt6370_pmu_rgbled_init_register(rgbled_data);
	if (ret < 0)
		goto out_init_reg;

	for (i = 0; i < ARRAY_SIZE(mt6370_pmu_led_trigger); i++) {
		ret = led_trigger_register(&mt6370_pmu_led_trigger[i]);
		if (ret < 0) {
			dev_err(&pdev->dev, "register %d trigger fail\n", i);
			goto out_led_trigger;
		}
	}

	for (i = 0; i < ARRAY_SIZE(mt6370_led_classdev); i++) {
		mt6370_led_classdev[i].led_dev.name = pdata->led_name[i];
		mt6370_led_classdev[i].led_dev.default_trigger =
						pdata->led_default_trigger[i];
		ret = led_classdev_register(&pdev->dev,
					    &mt6370_led_classdev[i].led_dev);
		if (ret < 0) {
			dev_err(&pdev->dev, "register led %d fail\n", i);
			goto out_led_register;
		}
	}

	mt6370_pmu_rgbled_irq_register(pdev);
	dev_info(&pdev->dev, "%s successfully\n", __func__);
	return 0;
out_led_register:
	while (--i >= 0)
		led_classdev_unregister(&mt6370_led_classdev[i].led_dev);
	i = ARRAY_SIZE(mt6370_pmu_led_trigger);
out_led_trigger:
	while (--i >= 0)
		led_trigger_register(&mt6370_pmu_led_trigger[i]);
out_init_reg:
out_init_data:
out_pdata:
	devm_kfree(&pdev->dev, rgbled_data);
	return ret;
}

static int mt6370_pmu_rgbled_remove(struct platform_device *pdev)
{
	struct mt6370_pmu_rgbled_data *rgbled_data = platform_get_drvdata(pdev);
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(mt6370_led_classdev); i++)
		led_classdev_unregister(&mt6370_led_classdev[i].led_dev);
	for (i = 0; i < ARRAY_SIZE(mt6370_pmu_led_trigger); i++)
		led_trigger_register(&mt6370_pmu_led_trigger[i]);
	dev_info(rgbled_data->dev, "%s successfully\n", __func__);
	return 0;
}

static const struct of_device_id mt_ofid_table[] = {
	{ .compatible = "mediatek,mt6370_pmu_rgbled", },
	{ },
};
MODULE_DEVICE_TABLE(of, mt_ofid_table);

static const struct platform_device_id mt_id_table[] = {
	{ "mt6370_pmu_rgbled", 0},
	{ },
};
MODULE_DEVICE_TABLE(platform, mt_id_table);

static struct platform_driver mt6370_pmu_rgbled = {
	.driver = {
		.name = "mt6370_pmu_rgbled",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(mt_ofid_table),
	},
	.probe = mt6370_pmu_rgbled_probe,
	.remove = mt6370_pmu_rgbled_remove,
	.id_table = mt_id_table,
};
module_platform_driver(mt6370_pmu_rgbled);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek MT6370 PMU RGBled");
MODULE_VERSION("1.0.0_G");
