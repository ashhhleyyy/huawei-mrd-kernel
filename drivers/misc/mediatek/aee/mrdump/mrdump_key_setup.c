/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/module.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/io.h>           /* ioremap() */
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_irq.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#endif
#ifdef CONFIG_MTK_WATCHDOG_COMMON
#include <mt-plat/mtk_wd_api.h>
#ifdef CONFIG_MTK_PMIC_COMMON
#include <mt-plat/upmu_common.h>
#endif
#else
#include <mach/wd_api.h>
#endif

enum MRDUMP_RST_SOURCE {
	MRDUMP_SYSRST,
	MRDUMP_EINT};

#ifdef CONFIG_MTK_PMIC_COMMON
enum MRDUMP_LONG_PRESS_MODE {
	LONG_PRESS_NONE,
	LONG_PRESS_SHUTDOWN};
enum MRDUMP_LONG_PRESS_KEY_MODE {
	KEY_NONE,
	POWER_ONLY,
	POWER_HOME};
#endif

static const struct of_device_id mrdump_key_of_ids[] = {
	{ .compatible = "mediatek, mrdump_ext_rst-eint", },
	{}
};

static int __init mrdump_key_probe(struct platform_device *pdev)
{
#ifdef CONFIG_MTK_WATCHDOG_COMMON
	int res;
	struct wd_api *wd_api = NULL;
#endif
	enum MRDUMP_RST_SOURCE source = MRDUMP_EINT;
	enum wk_req_mode mode = WD_REQ_IRQ_MODE;
#ifdef CONFIG_MTK_PMIC_COMMON
	enum MRDUMP_LONG_PRESS_MODE long_press_mode
			= LONG_PRESS_NONE;
	enum MRDUMP_LONG_PRESS_KEY_MODE key_combine_mode
			= KEY_NONE;
	const char *long_press;
	const char *key_combine;
#endif
	struct device_node *node;
	const char *source_str, *mode_str, *interrupts;
	char node_name[] = "mediatek, mrdump_ext_rst-eint";
	pr_notice("%s:%d\n", __func__, __LINE__);
	node = of_find_compatible_node(NULL, NULL, node_name);
	if (!node) {
		pr_notice("MRDUMP_KEY:node %s is not exist\n", node_name);
		goto out;
	}

	if (of_property_read_string(node, "interrupts", &interrupts)) {
		pr_notice("mrdump_key:no interrupts attribute from dws config\n");
		goto out;
	}

	if (!of_property_read_string(node, "source", &source_str)) {
		if (strcmp(source_str, "SYSRST") == 0) {
			source = MRDUMP_SYSRST;
#ifdef CONFIG_MTK_PMIC_COMMON
			if (!of_property_read_string(node, "long_press",
				&long_press)) {
				if (strcmp(long_press, "SHUTDOWN") == 0)
					long_press_mode = LONG_PRESS_SHUTDOWN;
				else if (strcmp(long_press, "NONE") == 0)
					long_press_mode = LONG_PRESS_NONE;
				else
					pr_info("long_press=%s not supported\n",
					long_press);
			}

			if (!of_property_read_string(node, "key_combination",
				&key_combine)) {
				if (strcmp(key_combine, "POWER_ONLY") == 0)
					key_combine_mode = POWER_ONLY;
				else if (strcmp(key_combine, "POWER_HOME") == 0)
					key_combine_mode = POWER_HOME;
				else
					key_combine_mode = KEY_NONE;
			}
#endif
		}
	} else
		pr_notice("MRDUMP_KEY:No attribute \"source\",  default to EINT\n");


	if (!of_property_read_string(node, "mode", &mode_str)) {
		if (strcmp(mode_str, "RST") == 0)
			mode = WD_REQ_RST_MODE;
	} else
		pr_notice("MRDUMP_KEY: no mode property,default IRQ");


#ifdef CONFIG_MTK_WATCHDOG_COMMON
	res = get_wd_api(&wd_api);
	if (res < 0) {
		pr_notice("MRDUMP_KEY: get_wd_api failed:%d\n", res);
		goto out;
	}

	if (source == MRDUMP_SYSRST) {
		res = wd_api->wd_debug_key_sysrst_config(1, mode);
		if (res == -1)
			pr_notice("%s: sysrst failed\n", __func__);
		else
			pr_notice("%s: enable MRDUMP_KEY SYSRST mode\n"
					, __func__);
#ifdef CONFIG_MTK_PMIC_COMMON
		pr_notice("%s: configure PMIC for smart reset\n"
				, __func__);
		if (long_press_mode == LONG_PRESS_SHUTDOWN) {
			pr_notice("long_press_mode = SHUTDOWN\n");
			pmic_enable_smart_reset(1, 1);
		} else {
			pr_notice("long_press_mode = NONE\n");
			pmic_enable_smart_reset(1, 0);
		}

		if (key_combine_mode == POWER_ONLY) {
			pmic_config_interface(PMIC_RG_PWRKEY_RST_EN_ADDR
			, 0x1, PMIC_RG_PWRKEY_RST_EN_MASK,
			PMIC_RG_PWRKEY_RST_EN_SHIFT);
			pr_info("key_combine=POWER_ONLY\n");

		} else if (key_combine_mode == POWER_HOME) {
			pmic_config_interface(PMIC_RG_HOMEKEY_RST_EN_ADDR,
			0x1, PMIC_RG_HOMEKEY_RST_EN_MASK,
			PMIC_RG_HOMEKEY_RST_EN_SHIFT);
			pr_info("key_combine=POWER_HOME\n");
		} else {
			pr_info("key_combine_mode=%d not aply\n"
					, key_combine_mode);
		}
#endif

	} else if (source == MRDUMP_EINT) {
		res = wd_api->wd_debug_key_eint_config(1, mode);
		if (res == -1)
			pr_notice("%s: eint failed\n", __func__);
		else
			pr_notice("%s: enabled MRDUMP_KEY EINT mode\n"
					, __func__);
	} else {
		pr_notice("%s:source %d is not match\n"
			"disable MRDUMP_KEY\n", __func__, source);
	}
#endif
out:
	of_node_put(node);
	return 0;
}

static void mrdump_key_shutdown(struct platform_device *pdev)
{

#ifdef CONFIG_MTK_WATCHDOG_COMMON
	int res;
	struct wd_api *wd_api = NULL;
#endif

#ifdef CONFIG_MTK_PMIC_COMMON
	pr_notice("restore pmic long_press_mode = SHUTDOWN\n");
	pmic_enable_smart_reset(0, 0);
#endif

#ifdef CONFIG_MTK_WATCHDOG_COMMON
	pr_notice("restore RGU to default value\n");
	res = get_wd_api(&wd_api);
	if (res < 0)
		pr_notice("%s: get_wd_api failed:%d\n", __func__, res);
	else {
		res = wd_api->wd_debug_key_eint_config(0, WD_REQ_RST_MODE);
		if (res == -1)
			pr_notice("%s: disable EINT failed\n", __func__);
		else
			pr_notice("%s:disable EINT mode\n", __func__);
		res = wd_api->wd_debug_key_sysrst_config(0, WD_REQ_RST_MODE);
		if (res == -1)
			pr_notice("%s: disable SYSRST failed\n", __func__);
		else
			pr_notice("%s:disable SYSRST OK\n", __func__);
	}
#endif
}

static void __exit mrdump_key_exit(void)
{
	mrdump_key_shutdown(NULL);
}
static int mrdump_key_remove(struct platform_device *dev)
{
	mrdump_key_shutdown(NULL);
	return 0;
}

/* variable with __init* or __refdata (see linux/init.h) or */
/* name the variable *_template, *_timer, *_sht, *_ops, *_probe, */
/* *_probe_one, *_console */
static struct platform_driver mrdump_key_driver_probe = {
	.probe = mrdump_key_probe,
	.shutdown = mrdump_key_shutdown,
	.remove = mrdump_key_remove,
	.driver = {
		.name = "mrdump_key",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = mrdump_key_of_ids,
#endif
	},
};

static int __init mrdump_key_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&mrdump_key_driver_probe);
	if (ret)
		pr_err("mrdump_key init FAIL, ret 0x%x!!!\n", ret);

	return ret;
}


module_init(mrdump_key_init);
module_exit(mrdump_key_exit);

