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


#define pr_fmt(fmt) "[ALS/PS] " fmt

#include "alspshub.h"
#include <alsps.h>
#include <hwmsensor.h>
#include <SCP_sensorHub.h>
#include "SCP_power_monitor.h"
#include <linux/pm_wakeup.h>
#include "../../../auxadc/mtk_auxadc.h"
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include "sensor_para.h"

#define ALSPSHUB_DEV_NAME     "alsps_hub_pl"
#define BATTERY_ID_CHANNEL_2  2
#define MAX_USB_VAL 550000
#define MIN_USB_VAL 460000
#define ALS_MAGIC_NUM 0x5AFC
#define HW_VOL_VALUE  3000000

struct alspshub_ipi_data {
	struct work_struct init_done_work;
	atomic_t first_ready_after_boot;
	/*misc */
	atomic_t	als_suspend;
	atomic_t	ps_suspend;
	atomic_t	trace;
	atomic_t	scp_init_done;

	/*data */
	u16		als;
	u8		ps;
	int		ps_cali;
	atomic_t	als_cali;
	atomic_t	ps_thd_val_high;
	atomic_t	ps_thd_val_low;
	ulong		enable;
	ulong		pending_intr;
	bool als_factory_enable;
	bool ps_factory_enable;
	bool als_android_enable;
	bool ps_android_enable;
	struct wakeup_source ps_wake_lock;
};

static unsigned int g_usb_id_adc_channel = BATTERY_ID_CHANNEL_2;

enum product_name {
	HW_PRODUCT_NONE,
	HW_PRODUCT_JAT,
	HW_PRODUCT_MRD,
	HW_PRODUCT_AMN,
	HW_PRODUCT_KSA,
	HW_PRODUCT_MAX,
};

#define HW_ALS_NAME_LEN    50
#define HW_ALS_PARAM_LEN   50
struct alspshub_als_param
{
	unsigned int tp_type;
	unsigned int tp_color;
	unsigned int product_type;
	unsigned char als_name[HW_ALS_NAME_LEN];
	unsigned short als_param[HW_ALS_PARAM_LEN];
};

struct alspshub_als_param alsps_als_param[] =
{
	{
		.tp_type = TS_PANEL_OFILIM,
		.tp_color = 0,
		.product_type = HW_PRODUCT_JAT,
		.als_name = "bh1726",
		.als_param = {1712, 2200, 5495, 5495, 315, 91, 235, 50, 168, 26, 168, 26},
	},
	{
		.tp_type = TS_PANEL_EELY,
		.tp_color = 0,
		.product_type = HW_PRODUCT_JAT,
		.als_name = "bh1726",
		.als_param = {772, 939, 3588, 3588, 414, 221, 508, 342, 235, 69, 235, 69},
	},
	{
		.tp_type = TS_PANEL_OFILIM,
		.tp_color = 0,
		.product_type = HW_PRODUCT_AMN,
		.als_name = "stk3338_als",
		.als_param = {7998, 259, 748, 1707, 292, 175, 130},
	},
	{
		.tp_type = TS_PANEL_EELY,
		.tp_color = 0,
		.product_type = HW_PRODUCT_AMN,
		.als_name = "stk3338_als",
		.als_param = {7657, 2835, 769, 1742, 340, 189, 160},
	},
	{
		.tp_type = TS_PANEL_TOPTOUCH,
		.tp_color = 0,
		.product_type = HW_PRODUCT_AMN,
		.als_name = "stk3338_als",
		.als_param = {7657, 2835, 769, 1742, 340, 189, 160},
	},
	{
		.tp_type = TS_PANEL_OFILIM,
		.tp_color = 0,
		.product_type = HW_PRODUCT_AMN,
		.als_name = "ltr2568_als",
		.als_param = {1978, 259, 1267, 4759, 1245, 1005,
			1085, 2660, 7500},
	},
	{
		.tp_type = TS_PANEL_EELY,
		.tp_color = 0,
		.product_type = HW_PRODUCT_AMN,
		.als_name = "ltr2568_als",
		.als_param = {1826, 267, 1403, 6179, 1700, 1027,
			1349, 2660, 8000},
	},
	{
		.tp_type = TS_PANEL_TOPTOUCH,
		.tp_color = 0,
		.product_type = HW_PRODUCT_AMN,
		.als_name = "ltr2568_als",
		.als_param = {1826, 267, 1403, 6179, 1700, 1027,
			1349, 2660, 8000},
	},
};

static unsigned int alspshub_product_type = 0;
static unsigned int alspshub_tp_type = 0;
static unsigned int alspshub_tp_color = 0;
static unsigned char alspshub_als_name[HW_ALS_NAME_LEN] = {0};
extern struct sensor_para_t *sensor_para;

static struct alspshub_ipi_data *obj_ipi_data;
static int ps_get_data(int *value, int *status);

static int alspshub_local_init(void);
static int alspshub_local_remove(void);
static int alshub_factory_enable_calibration(void);
static int alshub_factory_enable_sensor(bool enable_disable, int64_t sample_periods_ms);
#define FAC_POLL_TIME 200

static int alspshub_init_flag = -1;
static bool als_cali_status = false;
static struct alsps_init_info alspshub_init_info = {
	.name = "alsps_hub",
	.init = alspshub_local_init,
	.uninit = alspshub_local_remove,

};

static DEFINE_MUTEX(alspshub_mutex);
static DEFINE_SPINLOCK(calibration_lock);

enum {
	CMC_BIT_ALS = 1,
	CMC_BIT_PS = 2,
} CMC_BIT;
enum {
	CMC_TRC_ALS_DATA = 0x0001,
	CMC_TRC_PS_DATA = 0x0002,
	CMC_TRC_EINT = 0x0004,
	CMC_TRC_IOCTL = 0x0008,
	CMC_TRC_I2C = 0x0010,
	CMC_TRC_CVT_ALS = 0x0020,
	CMC_TRC_CVT_PS = 0x0040,
	CMC_TRC_DEBUG = 0x8000,
} CMC_TRC;

void ldo_regulator_init(void)
{
	struct regulator *regu_ldo = NULL;
	int ret;

	pr_info("%s: ++\n", __func__);
	/* Get ldo regulator handler */
	regu_ldo = regulator_get(NULL, "irtx_ldo");
	if (IS_ERR_OR_NULL(regu_ldo)) { /* handle return value */
		ret = PTR_ERR(regu_ldo);
		pr_err("%s: get irtx_ldo fail(%d)\n", __func__, ret);
		return;
	}
	/* Set ldo to 3V */
	ret = regulator_set_voltage(regu_ldo, HW_VOL_VALUE, HW_VOL_VALUE);
	if (ret < 0) {
		pr_err("%s: set ldo 3V fail(%d)\n", __func__, ret);
		regulator_put(regu_ldo);
		return;
	}
	/* Enable regulator */
	ret = regulator_enable(regu_ldo);
	if (ret < 0) {
		pr_info("%s: enable ldo fail(%d)\n", __func__, ret);
		regulator_put(regu_ldo);
		return;
	}

	regulator_put(regu_ldo);
	pr_info("%s: --\n", __func__);
}

long alspshub_read_ps(u8 *ps)
{
	long res;
	struct alspshub_ipi_data *obj = obj_ipi_data;
	struct data_unit_t data_t;

	res = sensor_get_data_from_hub(ID_PROXIMITY, &data_t);
	if (res < 0) {
		*ps = -1;
		pr_err("sensor_get_data_from_hub fail, (ID: %d)\n",
			ID_PROXIMITY);
		return -1;
	}
	if (data_t.proximity_t.steps < obj->ps_cali)
		*ps = 0;
	else
		*ps = data_t.proximity_t.steps - obj->ps_cali;
	return 0;
}

long alspshub_read_als(u16 *als)
{
	long res = 0;
	struct data_unit_t data_t;

	res = sensor_get_data_from_hub(ID_LIGHT, &data_t);
	if (res < 0) {
		*als = -1;
		pr_err_ratelimited("sensor_get_data_from_hub fail, (ID: %d)\n",
			ID_LIGHT);
		return -1;
	}
	*als = data_t.light;

	return 0;
}

static ssize_t alspshub_show_trace(struct device_driver *ddri, char *buf)
{
	ssize_t res = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (!obj_ipi_data) {
		pr_err("obj_ipi_data is null!!\n");
		return 0;
	}

	res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));
	return res;
}

static ssize_t alspshub_store_trace(struct device_driver *ddri,
				const char *buf, size_t count)
{
	int trace = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;
	int res = 0;
	int ret = 0;

	if (!obj) {
		pr_err("obj_ipi_data is null!!\n");
		return 0;
	}
	ret = sscanf(buf, "0x%x", &trace);
	if (ret != 1) {
		pr_err("invalid content: '%s', length = %zu\n", buf, count);
		return count;
	}
	atomic_set(&obj->trace, trace);
	res = sensor_set_cmd_to_hub(ID_PROXIMITY,
		CUST_ACTION_SET_TRACE, &trace);
	if (res < 0) {
		pr_err("sensor_set_cmd_to_hub fail,(ID: %d),(action: %d)\n",
			ID_PROXIMITY, CUST_ACTION_SET_TRACE);
		return 0;
	}
	return count;
}

static ssize_t alspshub_show_als(struct device_driver *ddri, char *buf)
{
	int res = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (!obj) {
		pr_err("obj_ipi_data is null!!\n");
		return 0;
	}
	res = alspshub_read_als(&obj->als);
	if (res)
		return snprintf(buf, PAGE_SIZE, "ERROR: %d\n", res);
	else
		return snprintf(buf, PAGE_SIZE, "0x%04X\n", obj->als);
}

static ssize_t alspshub_show_ps(struct device_driver *ddri, char *buf)
{
	ssize_t res = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (!obj) {
		pr_err("cm3623_obj is null!!\n");
		return 0;
	}
	res = alspshub_read_ps(&obj->ps);
	if (res)
		return snprintf(buf, PAGE_SIZE, "ERROR: %d\n", (int)res);
	else
		return snprintf(buf, PAGE_SIZE, "0x%04X\n", obj->ps);
}

static ssize_t alspshub_show_reg(struct device_driver *ddri, char *buf)
{
	int res = 0;

	res = sensor_set_cmd_to_hub(ID_PROXIMITY, CUST_ACTION_SHOW_REG, buf);
	if (res < 0) {
		pr_err("sensor_set_cmd_to_hub fail,(ID: %d),(action: %d)\n",
			ID_PROXIMITY, CUST_ACTION_SHOW_REG);
		return 0;
	}

	return res;
}

static ssize_t alspshub_show_alslv(struct device_driver *ddri, char *buf)
{
	int res = 0;

	res = sensor_set_cmd_to_hub(ID_LIGHT, CUST_ACTION_SHOW_ALSLV, buf);
	if (res < 0) {
		pr_err("sensor_set_cmd_to_hub fail,(ID: %d),(action: %d)\n",
			ID_LIGHT, CUST_ACTION_SHOW_ALSLV);
		return 0;
	}

	return res;
}

static ssize_t alspshub_show_alsval(struct device_driver *ddri, char *buf)
{
	int res = 0;

	res = sensor_set_cmd_to_hub(ID_LIGHT, CUST_ACTION_SHOW_ALSVAL, buf);
	if (res < 0) {
		pr_err("sensor_set_cmd_to_hub fail,(ID: %d),(action: %d)\n",
			ID_LIGHT, CUST_ACTION_SHOW_ALSVAL);
		return 0;
	}

	return res;
}

static ssize_t alspshub_store_alscali(struct device_driver *ddri,
				const char *buf, size_t count)
{
	//struct alspshub_ipi_data *obj = obj_ipi_data;
	int enable = 0, ret = 0, fool_proof_vol = 0;
	als_cali_status = false;
	if(NULL == buf){
		pr_err("%s, buf NULL error\n", __func__);
		return 0;
	}
	//acc_cali_flag = false;
	ret = kstrtoint(buf, 10, &enable);//change to 10-type
	if (ret != 0) {
		pr_err("kstrtoint fail\n");
		return 0;
	}
	pr_info("%s, cali cmd =%d\n", __func__, enable);
	ret = IMM_GetOneChannelValue_Cali(g_usb_id_adc_channel, &fool_proof_vol);
	if (ret != 0)
	{
		pr_err("get fool_proof_vol fail\n");
		return -1;
	}
	pr_info("%s, get fool_proof_vol =%d\n", __func__, fool_proof_vol);
	if(fool_proof_vol > MAX_USB_VAL || fool_proof_vol < MIN_USB_VAL){
		pr_err("%s out of usb voltage range\n", __func__);
		return -1;
	}
	if (enable == 1){
		alshub_factory_enable_sensor(true, FAC_POLL_TIME);//set to default 200ms
		alshub_factory_enable_calibration();
	}
	return count;
}

static ssize_t alspshub_show_alscali(struct device_driver *ddri, char *buf)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (!obj || !buf) {
		pr_err("obj_ipi_data is null!!\n");
		return 0;
	}
	//res = alspshub_read_als(&obj->als);
	//alshub_factory_enable_sensor(false, 0);
	if(als_cali_status == true){
		pr_info("return als calibrate result = PASS\n");
		return snprintf(buf, PAGE_SIZE, "%d\n", 0);
	}else{
		pr_info("return als calibrate result = FAIL\n");
		return snprintf(buf, PAGE_SIZE, "%d\n", 1);
	}

}

static DRIVER_ATTR(als, 0644, alspshub_show_als, NULL);
static DRIVER_ATTR(ps, 0644, alspshub_show_ps, NULL);
static DRIVER_ATTR(alslv, 0644, alspshub_show_alslv, NULL);
static DRIVER_ATTR(alsval, 0644, alspshub_show_alsval, NULL);
static DRIVER_ATTR(trace, 0644, alspshub_show_trace,
					alspshub_store_trace);
static DRIVER_ATTR(reg, 0644, alspshub_show_reg, NULL);
static DRIVER_ATTR(alscali, 0664, alspshub_show_alscali, alspshub_store_alscali);//set type 0664

static struct driver_attribute *alspshub_attr_list[] = {
	&driver_attr_als,
	&driver_attr_ps,
	&driver_attr_trace,	/*trace log */
	&driver_attr_alslv,
	&driver_attr_alsval,
	&driver_attr_reg,
	&driver_attr_alscali,
};

static int alspshub_create_attr(struct device_driver *driver)
{
	int idx = 0, err = 0;
	int num = (int)(ARRAY_SIZE(alspshub_attr_list));

	if (driver == NULL)
		return -EINVAL;

	for (idx = 0; idx < num; idx++) {
		err = driver_create_file(driver, alspshub_attr_list[idx]);
		if (err) {
			pr_err("driver_create_file (%s) = %d\n",
				alspshub_attr_list[idx]->attr.name, err);
			break;
		}
	}
	return err;
}

static int alspshub_delete_attr(struct device_driver *driver)
{
	int idx = 0, err = 0;
	int num = (int)(ARRAY_SIZE(alspshub_attr_list));

	if (!driver)
		return -EINVAL;

	for (idx = 0; idx < num; idx++)
		driver_remove_file(driver, alspshub_attr_list[idx]);

	return err;
}

static void alspshub_init_done_work(struct work_struct *work)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;
	int err = 0;
#ifndef MTK_OLD_FACTORY_CALIBRATION
	int32_t cfg_data[2] = {0};
#endif

	static bool isfirst = true;

	pr_err("%s, product_type: %d\n", __func__, alspshub_product_type);
	/* JAT: 1, MRD: 2 */
	if (isfirst && ((alspshub_product_type == 1) ||
		(alspshub_product_type == 2))) {
		ldo_regulator_init();
		isfirst = false;
	}

	if (atomic_read(&obj->scp_init_done) == 0) {
		pr_err("wait for nvram to set calibration\n");
		return;
	}
	if (atomic_xchg(&obj->first_ready_after_boot, 1) == 0)
		return;
#ifdef MTK_OLD_FACTORY_CALIBRATION
	err = sensor_set_cmd_to_hub(ID_PROXIMITY,
		CUST_ACTION_SET_CALI, &obj->ps_cali);
	if (err < 0)
		pr_err("sensor_set_cmd_to_hub fail,(ID: %d),(action: %d)\n",
			ID_PROXIMITY, CUST_ACTION_SET_CALI);
#else
	spin_lock(&calibration_lock);
	cfg_data[0] = atomic_read(&obj->ps_thd_val_low);
	cfg_data[1] = atomic_read(&obj->ps_thd_val_high);
	spin_unlock(&calibration_lock);
	err = sensor_cfg_to_hub(ID_PROXIMITY,
		(uint8_t *)cfg_data, sizeof(cfg_data));
	if (err < 0)
		pr_err("sensor_cfg_to_hub ps fail\n");

	spin_lock(&calibration_lock);
	cfg_data[0] = atomic_read(&obj->als_cali);
	spin_unlock(&calibration_lock);
	err = sensor_cfg_to_hub(ID_LIGHT,
		(uint8_t *)cfg_data, sizeof(cfg_data));
	if (err < 0)
		pr_err("sensor_cfg_to_hub als fail\n");
#endif
}
static int ps_recv_data(struct data_unit_t *event, void *reserved)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (!obj)
		return -1;

	if (event->flush_action == FLUSH_ACTION)
		ps_flush_report();
	else if (event->flush_action == DATA_ACTION &&
			READ_ONCE(obj->ps_android_enable) == true) {
		__pm_wakeup_event(&obj->ps_wake_lock, msecs_to_jiffies(100));
		ps_data_report(event->proximity_t.oneshot,
			SENSOR_STATUS_ACCURACY_HIGH);
	} else if (event->flush_action == CALI_ACTION) {
		spin_lock(&calibration_lock);
		atomic_set(&obj->ps_thd_val_high, event->data[0]);
		atomic_set(&obj->ps_thd_val_low, event->data[1]);
		spin_unlock(&calibration_lock);
		ps_cali_report(event->data);
	}
	return 0;
}
static int als_recv_data(struct data_unit_t *event, void *reserved)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (!obj)
		return -1;

	if (event->flush_action == FLUSH_ACTION)
		als_flush_report();
	else if ((event->flush_action == DATA_ACTION) &&
			READ_ONCE(obj->als_android_enable) == true)
		als_data_report(event->light, SENSOR_STATUS_ACCURACY_MEDIUM);
	else if (event->flush_action == CALI_ACTION) {
		pr_info("%s, recv als calibrate data\n", __func__);
		spin_lock(&calibration_lock);
		atomic_set(&obj->als_cali, event->data[0]);
		spin_unlock(&calibration_lock);
		pr_info("%s, recv als calibrate data = %d, resu = %d, ch0_ratio = %d, ch1_ratio = %d\n", __func__, event->data[0], (event->data[0] >> 31)&0x1, (event->data[0]) & 0xFFFF, (event->data[0] >> 16) & 0x7FFF);//0xFFFF,0x7FFF 16 for get cali data
		if(event->data[0] > 0){
			als_cali_status = true;
			als_cali_report(event->data);
		}
	}
	return 0;
}

static int rgbw_recv_data(struct data_unit_t *event, void *reserved)
{
	if (event->flush_action == FLUSH_ACTION)
		rgbw_flush_report();
	else if (event->flush_action == DATA_ACTION)
		rgbw_data_report(event->data);
	return 0;
}

static int alshub_factory_enable_sensor(bool enable_disable,
				int64_t sample_periods_ms)
{
	int err = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (enable_disable == true)
		WRITE_ONCE(obj->als_factory_enable, true);
	else
		WRITE_ONCE(obj->als_factory_enable, false);

	if (enable_disable == true) {
		err = sensor_set_delay_to_hub(ID_LIGHT, sample_periods_ms);
		if (err) {
			pr_err("sensor_set_delay_to_hub failed!\n");
			return -1;
		}
	}
	err = sensor_enable_to_hub(ID_LIGHT, enable_disable);
	if (err) {
		pr_err("sensor_enable_to_hub failed!\n");
		return -1;
	}
	mutex_lock(&alspshub_mutex);
	if (enable_disable)
		set_bit(CMC_BIT_ALS, &obj->enable);
	else
		clear_bit(CMC_BIT_ALS, &obj->enable);
	mutex_unlock(&alspshub_mutex);
	return 0;
}
static int alshub_factory_get_data(int32_t *data)
{
	int err = 0;
	struct data_unit_t data_t;

	err = sensor_get_data_from_hub(ID_LIGHT, &data_t);
	if (err < 0)
		return -1;
	*data = data_t.light;
	return 0;
}
static int alshub_factory_get_raw_data(int32_t *data)
{
	return alshub_factory_get_data(data);
}
static int alshub_factory_enable_calibration(void)
{
	return sensor_calibration_to_hub(ID_LIGHT);
}
static int alshub_factory_clear_cali(void)
{
	return 0;
}
static int alshub_factory_set_cali(int32_t offset)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;
	int err = 0;
	int32_t cfg_data;

	cfg_data = offset;
	err = sensor_cfg_to_hub(ID_LIGHT,
		(uint8_t *)&cfg_data, sizeof(cfg_data));
	if (err < 0)
		pr_err("sensor_cfg_to_hub fail\n");
	atomic_set(&obj->als_cali, offset);
	als_cali_report(&cfg_data);

	return err;

}
static int alshub_factory_get_cali(int32_t *offset)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	*offset = atomic_read(&obj->als_cali);
	return 0;
}
static int pshub_factory_enable_sensor(bool enable_disable,
			int64_t sample_periods_ms)
{
	int err = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	if (enable_disable == true) {
		err = sensor_set_delay_to_hub(ID_PROXIMITY, sample_periods_ms);
		if (err) {
			pr_err("sensor_set_delay_to_hub failed!\n");
			return -1;
		}
	}
	err = sensor_enable_to_hub(ID_PROXIMITY, enable_disable);
	if (err) {
		pr_err("sensor_enable_to_hub failed!\n");
		return -1;
	}
	mutex_lock(&alspshub_mutex);
	if (enable_disable)
		set_bit(CMC_BIT_PS, &obj->enable);
	else
		clear_bit(CMC_BIT_PS, &obj->enable);
	mutex_unlock(&alspshub_mutex);
	return 0;
}
static int pshub_factory_get_data(int32_t *data)
{
	int err = 0, status = 0;

	err = ps_get_data(data, &status);
	if (err < 0)
		return -1;
	return 0;
}
static int pshub_factory_get_raw_data(int32_t *data)
{
	int err = 0;
	struct data_unit_t data_t;

	err = sensor_get_data_from_hub(ID_PROXIMITY, &data_t);
	if (err < 0)
		return -1;
	*data = data_t.proximity_t.steps;
	return 0;
}
static int pshub_factory_enable_calibration(void)
{
	return sensor_calibration_to_hub(ID_PROXIMITY);
}
static int pshub_factory_clear_cali(void)
{
#ifdef MTK_OLD_FACTORY_CALIBRATION
	int err = 0;
#endif
	struct alspshub_ipi_data *obj = obj_ipi_data;

	obj->ps_cali = 0;
#ifdef MTK_OLD_FACTORY_CALIBRATION
	err = sensor_set_cmd_to_hub(ID_PROXIMITY,
			CUST_ACTION_RESET_CALI, &obj->ps_cali);
	if (err < 0) {
		pr_err("sensor_set_cmd_to_hub fail, (ID: %d),(action: %d)\n",
			ID_PROXIMITY, CUST_ACTION_RESET_CALI);
		return -1;
	}
#endif
	return 0;
}
static int pshub_factory_set_cali(int32_t offset)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	obj->ps_cali = offset;
	return 0;
}
static int pshub_factory_get_cali(int32_t *offset)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	*offset = obj->ps_cali;
	return 0;
}
static int pshub_factory_set_threshold(int32_t threshold[2])
{
	int err = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;
#ifndef MTK_OLD_FACTORY_CALIBRATION
	int32_t cfg_data[2] = {0};
#endif

	spin_lock(&calibration_lock);
	atomic_set(&obj->ps_thd_val_high, (threshold[0] + obj->ps_cali));
	atomic_set(&obj->ps_thd_val_low, (threshold[1] + obj->ps_cali));
	spin_unlock(&calibration_lock);
#ifdef MTK_OLD_FACTORY_CALIBRATION
	err = sensor_set_cmd_to_hub(ID_PROXIMITY,
		CUST_ACTION_SET_PS_THRESHOLD, threshold);
	if (err < 0)
		pr_err("sensor_set_cmd_to_hub fail, (ID:%d),(action:%d)\n",
			ID_PROXIMITY, CUST_ACTION_SET_PS_THRESHOLD);
#else
	spin_lock(&calibration_lock);
	cfg_data[0] = atomic_read(&obj->ps_thd_val_high);
	cfg_data[1] = atomic_read(&obj->ps_thd_val_low);
	spin_unlock(&calibration_lock);
	err = sensor_cfg_to_hub(ID_PROXIMITY,
		(uint8_t *)cfg_data, sizeof(cfg_data));
	if (err < 0)
		pr_err("sensor_cfg_to_hub fail\n");

	ps_cali_report(cfg_data);
#endif
	return err;
}

static int pshub_factory_get_threshold(int32_t threshold[2])
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	spin_lock(&calibration_lock);
	threshold[0] = atomic_read(&obj->ps_thd_val_high) - obj->ps_cali;
	threshold[1] = atomic_read(&obj->ps_thd_val_low) - obj->ps_cali;
	spin_unlock(&calibration_lock);
	return 0;
}

static struct alsps_factory_fops alspshub_factory_fops = {
	.als_enable_sensor = alshub_factory_enable_sensor,
	.als_get_data = alshub_factory_get_data,
	.als_get_raw_data = alshub_factory_get_raw_data,
	.als_enable_calibration = alshub_factory_enable_calibration,
	.als_clear_cali = alshub_factory_clear_cali,
	.als_set_cali = alshub_factory_set_cali,
	.als_get_cali = alshub_factory_get_cali,

	.ps_enable_sensor = pshub_factory_enable_sensor,
	.ps_get_data = pshub_factory_get_data,
	.ps_get_raw_data = pshub_factory_get_raw_data,
	.ps_enable_calibration = pshub_factory_enable_calibration,
	.ps_clear_cali = pshub_factory_clear_cali,
	.ps_set_cali = pshub_factory_set_cali,
	.ps_get_cali = pshub_factory_get_cali,
	.ps_set_threshold = pshub_factory_set_threshold,
	.ps_get_threshold = pshub_factory_get_threshold,
};

static struct alsps_factory_public alspshub_factory_device = {
	.gain = 1,
	.sensitivity = 1,
	.fops = &alspshub_factory_fops,
};
static int als_open_report_data(int open)
{
	return 0;
}

static bool als_get_param_success = false;
static unsigned short* als_get_param(void)
{
	unsigned int i = 0;
	for (i = 0; i < ARRAY_SIZE(alsps_als_param); i++)
	{
		if ((alsps_als_param[i].tp_type == alspshub_tp_type) && (alsps_als_param[i].tp_color == alspshub_tp_color) 
			&& (alsps_als_param[i].product_type == alspshub_product_type) 
			&& (0 == strncmp(alspshub_als_name, alsps_als_param[i].als_name, sizeof(alspshub_als_name))))
		{
			pr_info("als get param : %d\n", i);
			als_get_param_success = true;
			return alsps_als_param[i].als_param;
		}
	}
	pr_err("als get param err\n");
	return alsps_als_param[0].als_param;
}

static void als_params_to_scp(void)
{
	unsigned short *als_param = NULL;
	als_param = als_get_param();

	if (NULL == sensor_para)
	{
		pr_err("als_params_to_scp: sensor_para is NULL!\n");
		return ;
	}
	else
	{
		if (als_get_param_success == true) {
			sensor_para->als_para.para_magic_num = ALS_MAGIC_NUM;
			memcpy(sensor_para->als_para.als_extend_para, als_param,
			sizeof(sensor_para->als_para.als_extend_para));
		}
    }
}

static void als_get_name(void)
{
	int ret = 0;

	ret = sensor_set_cmd_to_hub(ID_LIGHT, CUST_ACTION_GET_SENSOR_INFO, alspshub_als_name);
	if (ret < 0)
	{
		pr_err("set_cmd_to_hub fail, (ID: %d),(action: %d)\n",
				ID_LIGHT, CUST_ACTION_GET_SENSOR_INFO);
	}

	pr_info("als name : %s\n", alspshub_als_name);
}
static int als_enable_nodata(int en)
{
	int res = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;
	static bool b_first_enable = true;

	pr_info("obj_ipi_data als enable value = %d\n", en);

	if (en == true)
		WRITE_ONCE(obj->als_android_enable, true);
	else
		WRITE_ONCE(obj->als_android_enable, false);

	if (b_first_enable)
	{
		als_get_name();
		als_params_to_scp();
		b_first_enable = false;
	}

	res = sensor_enable_to_hub(ID_LIGHT, en);
	if (res < 0) {
		pr_err("als_enable_nodata is failed!!\n");
		return -1;
	}

	mutex_lock(&alspshub_mutex);
	if (en)
		set_bit(CMC_BIT_ALS, &obj_ipi_data->enable);
	else
		clear_bit(CMC_BIT_ALS, &obj_ipi_data->enable);
	mutex_unlock(&alspshub_mutex);
	return 0;
}

static int als_set_delay(u64 ns)
{
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	int err = 0;
	unsigned int delayms = 0;

	delayms = (unsigned int)ns / 1000 / 1000;
	err = sensor_set_delay_to_hub(ID_LIGHT, delayms);
	if (err) {
		pr_err("als_set_delay fail!\n");
		return err;
	}
	pr_info("als_set_delay (%d)\n", delayms);
	return 0;
#elif defined CONFIG_NANOHUB
	return 0;
#else
	return 0;
#endif
}
static int als_batch(int flag,
	int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs)
{
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	als_set_delay(samplingPeriodNs);
#endif
	return sensor_batch_to_hub(ID_LIGHT, flag,
		samplingPeriodNs, maxBatchReportLatencyNs);
}

static int als_flush(void)
{
	return sensor_flush_to_hub(ID_LIGHT);
}

static int als_set_cali(uint8_t *data, uint8_t count)
{
	int32_t *buf = (int32_t *)data;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	spin_lock(&calibration_lock);
	atomic_set(&obj->als_cali, buf[0]);
	pr_info("als_set_cali cali data = %d, %d\n", buf[0] & 0xFFFF, (buf[0] >> 16)&0xFFFF);//0xFFFF, 0xFFFF, 16 set for get cali data
	spin_unlock(&calibration_lock);
	return sensor_cfg_to_hub(ID_LIGHT, data, count);
}

static int rgbw_enable(int en)
{
	int res = 0;

	res = sensor_enable_to_hub(ID_RGBW, en);
	if (res < 0) {
		pr_err("rgbw_enable is failed!!\n");
		return -1;
	}
	return 0;
}

static int rgbw_batch(int flag, int64_t samplingPeriodNs,
		int64_t maxBatchReportLatencyNs)
{
	return sensor_batch_to_hub(ID_RGBW,
		flag, samplingPeriodNs, maxBatchReportLatencyNs);
}

static int rgbw_flush(void)
{
	return sensor_flush_to_hub(ID_RGBW);
}

static int als_get_data(int *value, int *status)
{
	int err = 0;
	struct data_unit_t data;
	uint64_t time_stamp = 0;

	err = sensor_get_data_from_hub(ID_LIGHT, &data);
	if (err) {
		pr_err("sensor_get_data_from_hub fail!\n");
	} else {
		time_stamp = data.time_stamp;
		*value = data.light;
		*status = SENSOR_STATUS_ACCURACY_MEDIUM;
	}

	if (atomic_read(&obj_ipi_data->trace) & CMC_TRC_PS_DATA)
		pr_debug("value = %d\n", *value);
	return 0;
}

static int ps_open_report_data(int open)
{
	return 0;
}

static int ps_enable_nodata(int en)
{
	int res = 0;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	pr_info("obj_ipi_data ps enable value = %d\n", en);
	if (en == true)
		WRITE_ONCE(obj->ps_android_enable, true);
	else
		WRITE_ONCE(obj->ps_android_enable, false);

	res = sensor_enable_to_hub(ID_PROXIMITY, en);
	if (res < 0) {
		pr_err("als_enable_nodata is failed!!\n");
		return -1;
	}

	mutex_lock(&alspshub_mutex);
	if (en)
		set_bit(CMC_BIT_PS, &obj_ipi_data->enable);
	else
		clear_bit(CMC_BIT_PS, &obj_ipi_data->enable);
	mutex_unlock(&alspshub_mutex);


	return 0;

}

static int ps_set_delay(u64 ns)
{
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	int err = 0;
	unsigned int delayms = 0;

	delayms = (unsigned int)ns / 1000 / 1000;
	err = sensor_set_delay_to_hub(ID_PROXIMITY, delayms);
	if (err < 0) {
		pr_err("ps_set_delay fail!\n");
		return err;
	}

	pr_debug("ps_set_delay (%d)\n", delayms);
	return 0;
#elif defined CONFIG_NANOHUB
	return 0;
#else
	return 0;
#endif
}
static int ps_batch(int flag,
	int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs)
{
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	ps_set_delay(samplingPeriodNs);
#endif
	return sensor_batch_to_hub(ID_PROXIMITY, flag,
		samplingPeriodNs, maxBatchReportLatencyNs);
}

static int ps_flush(void)
{
	return sensor_flush_to_hub(ID_PROXIMITY);
}

static int ps_get_data(int *value, int *status)
{
	int err = 0;
	struct data_unit_t data;
	uint64_t time_stamp = 0;

	err = sensor_get_data_from_hub(ID_PROXIMITY, &data);
	if (err < 0) {
		pr_err("sensor_get_data_from_hub fail!\n");
		*value = -1;
		err = -1;
	} else {
		time_stamp = data.time_stamp;
		*value = data.proximity_t.oneshot;
		*status = SENSOR_STATUS_ACCURACY_MEDIUM;
	}

	if (atomic_read(&obj_ipi_data->trace) & CMC_TRC_PS_DATA)
		pr_debug("value = %d\n", *value);

	return err;
}

static int ps_set_cali(uint8_t *data, uint8_t count)
{
	int32_t *buf = (int32_t *)data;
	struct alspshub_ipi_data *obj = obj_ipi_data;

	spin_lock(&calibration_lock);
	atomic_set(&obj->ps_thd_val_low, buf[0]);
	atomic_set(&obj->ps_thd_val_high, buf[1]);
	spin_unlock(&calibration_lock);
	return sensor_cfg_to_hub(ID_PROXIMITY, data, count);
}

static int scp_ready_event(uint8_t event, void *ptr)
{
	struct alspshub_ipi_data *obj = obj_ipi_data;

	switch (event) {
	case SENSOR_POWER_UP:
	    atomic_set(&obj->scp_init_done, 1);
		schedule_work(&obj->init_done_work);
		break;
	case SENSOR_POWER_DOWN:
	    atomic_set(&obj->scp_init_done, 0);
		break;
	}
	return 0;
}

static struct scp_power_monitor scp_ready_notifier = {
	.name = "alsps",
	.notifier_call = scp_ready_event,
};

static void alspshub_get_chip_type(void)
{
	struct device_node* scpinfo_node = NULL;
	unsigned int product_type = 0;
	int ret = 0;

	if (sensor_para == NULL) {
		pr_err("sensor_para is NULL!\n");
		return;
	}
	scpinfo_node = of_find_compatible_node(NULL, NULL, "huawei,huawei_scp_info");

	if (NULL == scpinfo_node)
	{
		sensor_para->ps_para.product_name = HW_PRODUCT_NONE;
		pr_err("Cannot find huawei_scp_info from dts\n");
		return;
	}
	else
	{
		ret = of_property_read_u32(scpinfo_node, "product_number", &product_type);
		if (!ret)
		{
			alspshub_product_type = product_type;
			sensor_para->ps_para.product_name = product_type;
			pr_info("find product_type success %d\n", alspshub_product_type);
		}
		else
		{
			sensor_para->ps_para.product_name = HW_PRODUCT_NONE;
			pr_err("Cannot find product_type from dts\n");
			return;
		}
	}

	return;
}

static int alspshub_probe(struct platform_device *pdev)
{
	struct alspshub_ipi_data *obj;
	struct platform_driver *paddr =
			alspshub_init_info.platform_diver_addr;

	int err = 0;
	struct als_control_path als_ctl = { 0 };
	struct als_data_path als_data = { 0 };
	struct ps_control_path ps_ctl = { 0 };
	struct ps_data_path ps_data = { 0 };
	struct device_node *psensor_node = NULL;
	int ret = -1;

	pr_debug("%s\n", __func__);
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		err = -ENOMEM;
		goto exit;
	}

	memset(obj, 0, sizeof(*obj));
	obj_ipi_data = obj;

	INIT_WORK(&obj->init_done_work, alspshub_init_done_work);

	platform_set_drvdata(pdev, obj);


	atomic_set(&obj->als_suspend, 0);
	atomic_set(&obj->scp_init_done, 0);
	atomic_set(&obj->first_ready_after_boot, 0);

	obj->enable = 0;
	obj->pending_intr = 0;
	obj->ps_cali = 0;
	atomic_set(&obj->ps_thd_val_low, 21);
	atomic_set(&obj->ps_thd_val_high, 28);
	WRITE_ONCE(obj->als_factory_enable, false);
	WRITE_ONCE(obj->als_android_enable, false);
	WRITE_ONCE(obj->ps_factory_enable, false);
	WRITE_ONCE(obj->ps_android_enable, false);

	clear_bit(CMC_BIT_ALS, &obj->enable);
	clear_bit(CMC_BIT_PS, &obj->enable);
	scp_power_monitor_register(&scp_ready_notifier);
	err = scp_sensorHub_data_registration(ID_PROXIMITY, ps_recv_data);
	if (err < 0) {
		pr_err("scp_sensorHub_data_registration failed\n");
		goto exit_kfree;
	}
	err = scp_sensorHub_data_registration(ID_LIGHT, als_recv_data);
	if (err < 0) {
		pr_err("scp_sensorHub_data_registration failed\n");
		goto exit_kfree;
	}
	err = scp_sensorHub_data_registration(ID_RGBW, rgbw_recv_data);
	if (err < 0) {
		pr_err("scp_sensorHub_data_registration failed\n");
		goto exit_kfree;
	}
	err = alsps_factory_device_register(&alspshub_factory_device);
	if (err) {
		pr_err("alsps_factory_device_register register failed\n");
		goto exit_kfree;
	}
	pr_debug("alspshub_misc_device misc_register OK!\n");
	als_ctl.is_use_common_factory = false;
	ps_ctl.is_use_common_factory = false;
	err = alspshub_create_attr(&paddr->driver);
	if (err) {
		pr_err("create attribute err = %d\n", err);
		goto exit_create_attr_failed;
	}
	als_ctl.open_report_data = als_open_report_data;
	als_ctl.enable_nodata = als_enable_nodata;
	als_ctl.set_delay = als_set_delay;
	als_ctl.batch = als_batch;
	als_ctl.flush = als_flush;
	als_ctl.set_cali = als_set_cali;
	als_ctl.rgbw_enable = rgbw_enable;
	als_ctl.rgbw_batch = rgbw_batch;
	als_ctl.rgbw_flush = rgbw_flush;
	als_ctl.is_report_input_direct = false;

	als_ctl.is_support_batch = false;

	err = als_register_control_path(&als_ctl);
	if (err) {
		pr_err("register fail = %d\n", err);
		goto exit_create_attr_failed;
	}

	als_data.get_data = als_get_data;
	als_data.vender_div = 100;
	err = als_register_data_path(&als_data);
	if (err) {
		pr_err("tregister fail = %d\n", err);
		goto exit_create_attr_failed;
	}

	ps_ctl.open_report_data = ps_open_report_data;
	ps_ctl.enable_nodata = ps_enable_nodata;
	ps_ctl.set_delay = ps_set_delay;
	ps_ctl.batch = ps_batch;
	ps_ctl.flush = ps_flush;
	ps_ctl.set_cali = ps_set_cali;
	ps_ctl.is_report_input_direct = false;

	ps_ctl.is_support_batch = false;

	err = ps_register_control_path(&ps_ctl);
	if (err) {
		pr_err("register fail = %d\n", err);
		goto exit_create_attr_failed;
	}

	ps_data.get_data = ps_get_data;
	ps_data.vender_div = 100;
	err = ps_register_data_path(&ps_data);
	if (err) {
		pr_err("tregister fail = %d\n", err);
		goto exit_create_attr_failed;
	}
	wakeup_source_init(&obj->ps_wake_lock, "ps_wake_lock");

	alspshub_init_flag = 0;

	psensor_node = of_find_compatible_node(NULL, NULL, "mediatek,psensor");
	if(psensor_node){
		ret = of_property_read_u32(psensor_node, "usb_id_use_adc_channel", &g_usb_id_adc_channel);
		if(ret){
			g_usb_id_adc_channel = BATTERY_ID_CHANNEL_2;
		}
	}else{
		g_usb_id_adc_channel = BATTERY_ID_CHANNEL_2;
	}

	pr_debug("%s: OK\n", __func__);
	return 0;

exit_create_attr_failed:
	alspshub_delete_attr(&(alspshub_init_info.platform_diver_addr->driver));
exit_kfree:
	kfree(obj);
	obj_ipi_data = NULL;
exit:
	pr_err("%s: err = %d\n", __func__, err);
	alspshub_init_flag = -1;
	return err;
}

static int alspshub_remove(struct platform_device *pdev)
{
	int err = 0;
	struct platform_driver *paddr =
			alspshub_init_info.platform_diver_addr;

	err = alspshub_delete_attr(&paddr->driver);
	if (err)
		pr_err("alspshub_delete_attr fail: %d\n", err);
	alsps_factory_device_deregister(&alspshub_factory_device);
	kfree(platform_get_drvdata(pdev));
	return 0;

}

static int alspshub_suspend(struct platform_device *pdev, pm_message_t msg)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int alspshub_resume(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	return 0;
}
static struct platform_device alspshub_device = {
	.name = ALSPSHUB_DEV_NAME,
	.id = -1,
};

static struct platform_driver alspshub_driver = {
	.probe = alspshub_probe,
	.remove = alspshub_remove,
	.suspend = alspshub_suspend,
	.resume = alspshub_resume,
	.driver = {
		.name = ALSPSHUB_DEV_NAME,
	},
};

static int alspshub_local_init(void)
{

	if (platform_driver_register(&alspshub_driver)) {
		pr_err("add driver error\n");
		return -1;
	}
	if (-1 == alspshub_init_flag)
		return -1;
	return 0;
}
static int alspshub_local_remove(void)
{

	platform_driver_unregister(&alspshub_driver);
	return 0;
}

static DEFINE_MUTEX(mutex_set_para);
static BLOCKING_NOTIFIER_HEAD(tp_notifier_list);
int tpmodule_register_client(struct notifier_block* nb)
{
	return blocking_notifier_chain_register(&tp_notifier_list, nb);
}
EXPORT_SYMBOL(tpmodule_register_client);

int tpmodule_unregister_client(struct notifier_block* nb)
{
	return blocking_notifier_chain_unregister(&tp_notifier_list, nb);
}
EXPORT_SYMBOL(tpmodule_unregister_client);

int tpmodule_notifier_call_chain(unsigned long val, void* v)
{
	return blocking_notifier_call_chain(&tp_notifier_list, val, v);
}
EXPORT_SYMBOL(tpmodule_notifier_call_chain);

static int read_tp_module_notify(struct notifier_block* nb, unsigned long action, void* data)
{
	pr_info("%s, start!\n", __func__);

	mutex_lock(&mutex_set_para);
	alspshub_tp_type = action;
	mutex_unlock(&mutex_set_para);
	pr_info("%s, get tp module type = %d\n", __func__, alspshub_tp_type);

	return NOTIFY_OK;
}

static struct notifier_block readtp_notify =
{
	.notifier_call = read_tp_module_notify,
};

static int __init alspshub_init(void)
{
	pr_err("Enter %s\n", __func__);
	if (platform_device_register(&alspshub_device)) {
		pr_err("alsps platform device error\n");
		return -1;
	}
	alspshub_get_chip_type();
	tpmodule_register_client(&readtp_notify);
	alsps_driver_add(&alspshub_init_info);
	return 0;
}

static void __exit alspshub_exit(void)
{
	pr_debug("%s\n", __func__);
}

module_init(alspshub_init);
module_exit(alspshub_exit);
MODULE_AUTHOR("hongxu.zhao@mediatek.com");
MODULE_DESCRIPTION("alspshub driver");
MODULE_LICENSE("GPL");
