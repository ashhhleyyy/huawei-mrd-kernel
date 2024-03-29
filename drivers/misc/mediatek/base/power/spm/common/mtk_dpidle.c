/*
 * Copyright (C) 2017 MediaTek Inc.
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

#include <linux/kernel.h>
#include <linux/module.h>

#include <mtk_idle.h>
#include <mtk_idle_internal.h>
#include <mtk_spm_internal.h> /* mtk_idle_cond_check */

#include <mtk_idle_sysfs.h>

static bool dpidle_feature_enable = MTK_IDLE_FEATURE_ENABLE_DPIDLE;
static bool dpidle_bypass_idle_cond;
static bool dpidle_force_vcore_lp_mode;

unsigned long dp_cnt[NR_CPUS] = {0};
static unsigned long dp_block_cnt[NR_REASONS] = {0};
static unsigned int dpidle_flag = MTK_IDLE_LOG_REDUCE;

/* [ByChip] Internal weak functions: implemented in mtk_idle_cond_check.c */
void __attribute__((weak)) mtk_idle_cg_monitor(int sel) {}
bool __attribute__((weak)) mtk_idle_cond_check(int idle_type) {return false; }
void __attribute__((weak)) mtk_idle_cond_update_mask(
	int idle_type, unsigned int reg, unsigned int mask) {}
int __attribute__((weak)) mtk_idle_cond_append_info(
	bool short_log, int idle_type, char *logptr, unsigned int logsize);

bool mtk_dpidle_enabled(void)
{
	return dpidle_feature_enable;
}

void mtk_dpidle_disable(void)
{
	dpidle_feature_enable = 0;
}

/* mtk_dpidle_is_active() for pmic_throttling_dlpt
 *   return 0 : entering dpidle recently ( > 1s)
 *                      => normal mode(dlpt 10s)
 *   return 1 : entering dpidle recently (<= 1s)
 *                      => light-loading mode(dlpt 20s)
 */
#define DPIDLE_ACTIVE_TIME		(1)
static struct timeval pre_dpidle_time;
bool mtk_dpidle_is_active(void)
{
	struct timeval current_time;
	long int diff;

	do_gettimeofday(&current_time);
	diff = current_time.tv_sec - pre_dpidle_time.tv_sec;

	if (diff > DPIDLE_ACTIVE_TIME)
		return false;
	else if ((diff == DPIDLE_ACTIVE_TIME) &&
		(current_time.tv_usec > pre_dpidle_time.tv_usec))
		return false;
	else
		return true;
}
EXPORT_SYMBOL(mtk_dpidle_is_active);


bool dpidle_can_enter(int reason)
{
	/* dpidle is disabled */
	if (!dpidle_feature_enable)
		return false;

	if (reason < NR_REASONS)
		goto out;

	if (!dpidle_bypass_idle_cond && !mtk_idle_cond_check(IDLE_TYPE_DP)) {
		reason = BY_CLK;
		goto out;
	}

	reason = NR_REASONS;

out:
	return mtk_idle_select_state(IDLE_TYPE_DP, reason);
}

int dpidle_enter(int cpu)
{
	unsigned int op_cond = 0;

	if (dpidle_force_vcore_lp_mode)
		op_cond |= MTK_IDLE_OPT_VCORE_LP_MODE;

	lockdep_off();
	mtk_idle_ratio_calc_start(IDLE_TYPE_DP, cpu);
	mtk_idle_enter(IDLE_TYPE_DP, cpu, op_cond, dpidle_flag);
	mtk_idle_ratio_calc_stop(IDLE_TYPE_DP, cpu);
	lockdep_on();

	dp_cnt[cpu]++;

	do_gettimeofday(&pre_dpidle_time);

	return CPUIDLE_STATE_RG;
}
EXPORT_SYMBOL(dpidle_enter);

static ssize_t dpidle_state_read(char *ToUserBuf, size_t sz, void *priv)
{
	int i;
	char *p = ToUserBuf;

	#undef log
	#define log(fmt, args...) ({\
			p += scnprintf(p, sz - strlen(ToUserBuf)\
					, fmt, ##args); p; })

	log("*************** dpidle state *********************\n");
	for (i = 0; i < NR_REASONS; i++) {
		if (dp_block_cnt[i] > 0)
			log("[%d] block_cnt[%s]=%lu\n", i,
				mtk_idle_block_reason_name(i), dp_block_cnt[i]);
	}
	log("\n");

	p += mtk_idle_cond_append_info(
		false, IDLE_TYPE_DP, p, sz - strlen(ToUserBuf));
	log("\n");

	log("*************** variable dump ********************\n");
	log("enable=%d bypass=%d force_vcore_lp_mode=%d\n",
		dpidle_feature_enable ? 1 : 0,
		dpidle_bypass_idle_cond ? 1 : 0,
		dpidle_force_vcore_lp_mode ? 1 : 0);
	log("log_flag=%x\n", dpidle_flag);
	log("\n");

	log("*************** dpidle command *******************\n");
	log("dpidle state:  cat /d/cpuidle/dpidle_state\n");
	log("dpidle on/off: echo 1/0 > /d/cpuidle/dpidle_state\n");
	log("bypass cg/pll: echo bypass 1/0 > /d/cpuidle/dpidle_state\n");
	log("cg monitor:    echo cgmon 1/0 > /d/cpuidle/dpidle_state\n");
	log("set log_flag:  echo log [hex] > /d/cpuidle/dpidle_state\n");
	log("               [0] reduced [1] spm res usage [2] disable\n");
	log("\n");

	return p - ToUserBuf;
}

static ssize_t dpidle_state_write(char *FromUserBuf, size_t sz, void *priv)
{
	char cmd[128];
	int parm, parm1;


	if (sscanf(FromUserBuf, "%127s %x %x", cmd, &parm, &parm1) == 3) {
		if (!strcmp(cmd, "mask"))
			mtk_idle_cond_update_mask(IDLE_TYPE_DP, parm, parm1);
	} else if (sscanf(FromUserBuf, "%127s %x", cmd, &parm) == 2) {
		if (!strcmp(cmd, "dpidle"))
			dpidle_feature_enable = !!parm;
		else if (!strcmp(cmd, "bypass"))
			dpidle_bypass_idle_cond = !!parm;
		else if (!strcmp(cmd, "force_vcore_lp_mode"))
			dpidle_force_vcore_lp_mode = !!parm;
		else if (!strcmp(cmd, "cgmon"))
			mtk_idle_cg_monitor(parm ? IDLE_TYPE_DP : -1);
		else if (!strcmp(cmd, "log"))
			dpidle_flag = parm;
		return sz;
	} else if ((!kstrtoint(FromUserBuf, 10, &parm)) == 1) {
		dpidle_feature_enable = !!parm;
		return sz;
	}

	return -EINVAL;
}

static const struct mtk_idle_sysfs_op dpidle_state_fops = {
	.fs_read = dpidle_state_read,
	.fs_write = dpidle_state_write,
};

void mtk_dpidle_init(struct mtk_idle_init_data *pData)
{
	/*If dts node have status information then set state to dp*/
	if (pData) {
		if (IS_MTK_LP_DTS_AVAILABLE_DP(pData)) {
			dpidle_feature_enable =
				GET_MTK_LP_DTS_VALUE_DP(pData);
		}
	}
	dpidle_bypass_idle_cond = false;
	dpidle_force_vcore_lp_mode = false;
	mtk_idle_sysfs_entry_node_add("dpidle_state"
				, 0644, &dpidle_state_fops, NULL);

	mtk_idle_block_setting(IDLE_TYPE_DP, dp_cnt, dp_block_cnt);
}

