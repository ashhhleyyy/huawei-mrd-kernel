/*
 * Copyright (C) 2017 MediaTek Inc.
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
#ifndef __TMP_BTS_H__
#define __TMP_BTS_H__

/* chip dependent */

#define APPLY_PRECISE_NTC_TABLE
#define APPLY_AUXADC_CALI_DATA

#define AUX_IN0_NTC (0)
#define AUX_IN1_NTC (1)
#define AUX_IN3_NTC (3)
#define AUX_IN4_NTC (4)

#define BTS_RAP_PULL_UP_R		37400 /* 37.4K, pulli up resister */

#define BTS_TAP_OVER_CRITICAL_LOW	188500 /* base on 10K NTC temp
						 * default value -40 deg
						 */

#define BTS_RAP_PULL_UP_VOLTAGE		1800 /* 1.8V ,pull up voltage */

#define BTS_RAP_NTC_TABLE		8 /* NTCG103JF103F (10K) */

#define BTS_RAP_ADC_CHANNEL		AUX_IN0_NTC /* default is 0 */

#define BTSMDPA_RAP_PULL_UP_R		37400 /* 37.4K, pull up resister */

#define BTSMDPA_TAP_OVER_CRITICAL_LOW	188500 /* base on 10K NTC temp
						 * default value -40 deg
						 */

#define BTSMDPA_RAP_PULL_UP_VOLTAGE	1800 /* 1.8V ,pull up voltage */

#define BTSMDPA_RAP_NTC_TABLE		8 /* NTCG103JF103F (10K) */

#define BTSMDPA_RAP_ADC_CHANNEL		AUX_IN1_NTC /* default is 1 */


#define BTS_USBNTC_RAP_PULL_UP_R		37400 /* 37.4K, pull up resister */
#define BTS_USBNTC_TAP_OVER_CRITICAL_LOW	188500 /* base on 10K NTC temp
						 * default value -40 deg
						 */
#define BTS_USBNTC_RAP_PULL_UP_VOLTAGE		1800 /* 1.8V ,pull up voltage */
#define BTS_USBNTC_RAP_NTC_TABLE		8 /* NTCG103JF103F (10K) */
#define BTS_USBNTC_RAP_ADC_CHANNEL		AUX_IN3_NTC /* default is 0 */


#define BTS_CHARGERNTC_RAP_PULL_UP_R		37400 /* 37.4K, pull up resister */
#define BTS_CHARGERNTC_TAP_OVER_CRITICAL_LOW	188500 /* base on 10K NTC temp
						 * default value -40 deg
						 */
#define BTS_CHARGERNTC_RAP_PULL_UP_VOLTAGE		1800 /* 1.8V ,pull up voltage */
#define BTS_CHARGERNTC_RAP_NTC_TABLE		8 /* NTCG103JF103F (10K) */
#define BTS_CHARGERNTC_RAP_ADC_CHANNEL		AUX_IN4_NTC /* default is 0 */


extern int IMM_GetOneChannelValue(int dwChannel, int data[4], int *rawdata);
extern int IMM_IsAdcInitReady(void);

#endif	/* __TMP_BTS_H__ */
