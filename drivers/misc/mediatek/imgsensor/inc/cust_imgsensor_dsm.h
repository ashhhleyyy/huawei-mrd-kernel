/*
 *  Copyright (C) 2018 Huawei Inc.
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

#ifndef __CUST_IMGSENSOR_DSM_H
#define __CUST_IMGSENSOR_DSM_H


#ifdef CONFIG_HUAWEI_DSM
#include <dsm/dsm_pub.h>

#define DSM_BUFF_SIZE (256)
static struct dsm_dev dsm_camera = {
    .name = "dsm_camera",
    .fops = NULL,
    .buff_size = DSM_BUFF_SIZE,
};
void imgsensor_report_dsm_err( int err_code,const char* err_msg);
#endif

#endif
