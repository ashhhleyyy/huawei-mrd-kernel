

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <asm/atomic.h>
#include "kd_camera_typedef.h"
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_imgsensor_errcode.h"
#include "imgsensor_sensor_common.h"
#include "imgsensor_sensor_i2c.h"

#include "hi846_byd_sensor.h"

#define Hi846_MAXGAIN 16

#define RETRY_TIMES 2

/****************************Modify Following Strings for Debug****************************/
#define PFX "[hi846_byd]"
#define DEBUG_HI846_BYD 0
#define LOG_DBG(fmt, args...) \
	do { \
		if (DEBUG_HI846_BYD) \
			pr_debug(PFX "%s %d " fmt, __func__, __LINE__, ##args); \
	} while (0)
#define LOG_INF(fmt, args...) pr_info(PFX "%s %d " fmt, __func__, __LINE__, ##args)
#define LOG_ERR(fmt, args...) pr_err(PFX "%s %d " fmt, __func__, __LINE__, ##args)

/****************************   Modify end    *******************************************/

static DEFINE_SPINLOCK(imgsensor_drv_lock);

static void set_dummy(void)
{
	LOG_DBG("ENTER\n");
	(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_FRM_LENGTH_LINES_REG_H, imgsensor.frame_length,
					 IMGSENSOR_I2C_WORD_DATA);
	(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_LINE_LENGTH_PCK_REG_H, imgsensor.line_length,
					 IMGSENSOR_I2C_WORD_DATA);
} /* 	set_dummy  */

static kal_uint32 return_sensor_id(void)
{
	kal_int32 rc = 0;
	kal_uint16 sensor_id = 0;
	kal_uint16 sensor_id_l = 0;
	kal_uint16 sensor_id_h = 0;

	rc = imgsensor_sensor_i2c_read(&imgsensor, imgsensor_info.sensor_id_reg,
				       &sensor_id, IMGSENSOR_I2C_WORD_DATA);
	if (rc < 0) {
		LOG_ERR("Read id failed.id reg: 0x%x\n", imgsensor_info.sensor_id_reg);
		sensor_id = 0xFFFF;
	}

	sensor_id_h = sensor_id & 0xff;
	sensor_id_l = (sensor_id >> 8) & 0xff;
	sensor_id = (sensor_id_h << 8 | sensor_id_l);
	return sensor_id;
}
static void set_max_framerate(UINT16 framerate, kal_bool min_framelength_en)
{
	kal_uint32 frame_length = imgsensor.frame_length;
	LOG_DBG("ENTER\n");

	if (!framerate || !imgsensor.line_length) {
		LOG_ERR("Invalid params. framerate=%d, line_length=%d.\n",
			framerate, imgsensor.line_length);
		return;
	}
	frame_length = imgsensor.pclk / framerate * 10 / imgsensor.line_length;
	spin_lock(&imgsensor_drv_lock);
	imgsensor.frame_length = (frame_length > imgsensor.min_frame_length) ? frame_length : imgsensor.min_frame_length;
	imgsensor.dummy_line = imgsensor.frame_length - imgsensor.min_frame_length;

	if (imgsensor.frame_length > imgsensor_info.max_frame_length) {
		imgsensor.frame_length = imgsensor_info.max_frame_length;
		imgsensor.dummy_line = imgsensor.frame_length - imgsensor.min_frame_length;
	}
	if (min_framelength_en)
		imgsensor.min_frame_length = imgsensor.frame_length;
	spin_unlock(&imgsensor_drv_lock);
	set_dummy();
	return;
} /*  set_max_framerate  */

static void write_shutter(kal_uint16 shutter)
{
	kal_uint16 realtime_fps = 0;
	LOG_DBG("ENTER\n");

	spin_lock(&imgsensor_drv_lock);
	if (shutter > imgsensor.min_frame_length - imgsensor_info.margin)
		imgsensor.frame_length = shutter + imgsensor_info.margin;
	else
		imgsensor.frame_length = imgsensor.min_frame_length;
	if (imgsensor.frame_length > imgsensor_info.max_frame_length)
		imgsensor.frame_length = imgsensor_info.max_frame_length;
	spin_unlock(&imgsensor_drv_lock);
	shutter = (shutter < imgsensor_info.min_shutter) ? imgsensor_info.min_shutter : shutter;
	shutter = (shutter > (imgsensor_info.max_frame_length - imgsensor_info.margin)) ? (imgsensor_info.max_frame_length -
			imgsensor_info.margin) : shutter;

	if (imgsensor.autoflicker_en) {
		realtime_fps = imgsensor.pclk / imgsensor.line_length * 10 / imgsensor.frame_length;
		/* calc fps between 298~305, real fps set to 298 */
		if (realtime_fps >= 298 && realtime_fps <= 305)
			set_max_framerate(298, 0);
		/* calc fps between 147~150, real fps set to 146 */
		else if (realtime_fps >= 147 && realtime_fps <= 150)
			set_max_framerate(146, 0);
		else {
			(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_FRM_LENGTH_LINES_REG_H, imgsensor.frame_length,
							 IMGSENSOR_I2C_WORD_DATA);
		}
	} else {
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_FRM_LENGTH_LINES_REG_H, imgsensor.frame_length,
						 IMGSENSOR_I2C_WORD_DATA);
	}

	(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_INTEG_TIME_REG_H, shutter, IMGSENSOR_I2C_WORD_DATA);

	LOG_DBG("Exit! shutter =%d, framelength =%d\n", shutter, imgsensor.frame_length);
} /* 	write_shutter  */

/*************************************************************************
* FUNCTION
* 	set_shutter
*
* DESCRIPTION
* 	This function set e-shutter of sensor to change exposure time.
*
* PARAMETERS
* 	iShutter : exposured lines
*
* RETURNS
* 	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
static void set_shutter(kal_uint16 shutter)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&imgsensor_drv_lock, flags);
	imgsensor.shutter = shutter;
	spin_unlock_irqrestore(&imgsensor_drv_lock, flags);

	write_shutter(shutter);
} /* 	set_shutter */

static kal_uint16 gain2reg(const kal_uint16 gain)
{
	kal_uint16 reg_gain = 0x0000;
	reg_gain = gain / 4 - 16;

	return (kal_uint16)reg_gain;
}
/*************************************************************************
* FUNCTION
* 	set_gain
*
* DESCRIPTION
* 	This function is to set global gain to sensor.
*
* PARAMETERS
* 	iGain : sensor global gain(base: 0x40)
*
* RETURNS
* 	the actually gain set to sensor.
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint16 set_gain(kal_uint16 gain)
{
	kal_uint16 reg_gain;
	LOG_DBG("ENTER.\n");
	/* 0x350A[0:1], 0x350B[0:7] AGC real gain */
	/* [0:3] = N meams N /16 X	  */
	/* [4:9] = M meams M X		   */
	/* Total gain = M + N /16 X   */

	if (gain < BASEGAIN) {
		LOG_ERR("Invaild gain: %d\n", gain);
		gain = BASEGAIN;
	} else if (gain > Hi846_MAXGAIN * BASEGAIN) {
		LOG_ERR("Invaild gain: %d\n", gain);
		gain = Hi846_MAXGAIN * BASEGAIN;
	}

	reg_gain = gain2reg(gain);
	spin_lock(&imgsensor_drv_lock);
	imgsensor.gain = reg_gain;
	spin_unlock(&imgsensor_drv_lock);
	LOG_DBG("gain = %d , reg_gain = 0x%x\n ", gain, reg_gain);

	(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_ANA_GAIN_REG, (reg_gain & 0xFFFF), IMGSENSOR_I2C_WORD_DATA);
	LOG_DBG("EXIT.\n");

	return gain;

} /* 	set_gain  */

/*************************************************************************
* FUNCTION
* 	sensor_dump_reg
*
* DESCRIPTION
* 	This function dump some sensor reg
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint32 sensor_dump_reg(void)
{
	kal_int32 rc = 0;
	LOG_INF("ENTER\n");
	rc = imgsensor_sensor_i2c_process(&imgsensor, &imgsensor_info.dump_info);
	if (rc < 0)
		LOG_ERR("Failed.\n");
	LOG_INF("EXIT\n");
	return ERROR_NONE;
}

static void set_mirror_flip(kal_uint8 image_mirror)
{
	/********************************************************
	SENSOR_IMAGE_ORIENTATION:
		bit[7:2]: Reserved
		bit[1]: V Flip enable [0:no flip, 1:v flip]
		bit[0]: H mirror enable [0:no mirror, 1: H mirror]
	   *
	   ********************************************************/
	switch (image_mirror) {
	case IMAGE_NORMAL:
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_IMAGE_ORIENTATION, 0x0000, IMGSENSOR_I2C_WORD_DATA);
		break;
	case IMAGE_H_MIRROR:
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_IMAGE_ORIENTATION, 0x0100, IMGSENSOR_I2C_WORD_DATA);
		break;
	case IMAGE_V_MIRROR:
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_IMAGE_ORIENTATION, 0x0200, IMGSENSOR_I2C_WORD_DATA);
		break;
	case IMAGE_HV_MIRROR:
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_IMAGE_ORIENTATION, 0x0300, IMGSENSOR_I2C_WORD_DATA);
		break;
	default:
		LOG_INF("Error image_mirror setting");
	}
}

static void sensor_init(void)
{
	kal_int32 rc = 0;
	LOG_DBG("ENTER.\n");

	rc = imgsensor_sensor_write_setting(&imgsensor, &imgsensor_info.init_setting);
	if (rc < 0) {
		LOG_ERR("Failed.\n");
		return;
	}
	LOG_DBG("EXIT.\n");

	return;
}

static void set_preview_setting(void)
{
	kal_int32 rc = 0;
	LOG_DBG("ENTER\n");

	rc = imgsensor_sensor_write_setting(&imgsensor, &imgsensor_info.pre_setting);
	if (rc < 0) {
		LOG_ERR("Failed.\n");
		return;
	}
	LOG_DBG("EXIT.\n");

	return;
}

static void set_capture_setting(void)
{
	kal_int32 rc = 0;
	LOG_DBG("ENTER\n");

	rc = imgsensor_sensor_write_setting(&imgsensor, &imgsensor_info.cap_setting);
	if (rc < 0) {
		LOG_ERR("Failed.\n");
		return;
	}
	LOG_DBG("EXIT.\n");

	return;
}

static void set_normal_video_setting(void)
{
	kal_int32 rc = 0;
	LOG_DBG("ENTER\n");

	rc = imgsensor_sensor_write_setting(&imgsensor, &imgsensor_info.normal_video_setting);
	if (rc < 0) {
		LOG_ERR("Failed.\n");
		return;
	}

	LOG_DBG("EXIT\n");

	return;
}

/*************************************************************************
* FUNCTION
* 	get_imgsensor_id
*
* DESCRIPTION
* 	This function get the sensor ID
*
* PARAMETERS
* 	*sensorID : return the sensor ID
*
* RETURNS
* 	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint32 get_imgsensor_id(UINT32 *sensor_id)
{
	kal_uint8 i = 0;
	kal_uint8 retry = RETRY_TIMES; /* retry 2 time */
	UINT32 reg_sensor_id = 0;
	UINT32 tmp_sensor_id = 0;
	UINT32 vendorID_offset = 4; /* vendorID offset is 4 in eeprom */

	LOG_DBG("get_imgsensor_id.\n");

	spin_lock(&imgsensor_drv_lock);
	/* init i2c config */
	imgsensor.i2c_speed = imgsensor_info.i2c_speed;
	imgsensor.addr_type = imgsensor_info.addr_type;
	spin_unlock(&imgsensor_drv_lock);

	/* get sensorID from imagesensor_info */
	tmp_sensor_id = (imgsensor_info.sensor_id & 0x0ffff000) >> 12;

	while (imgsensor_info.i2c_addr_table[i] != 0xff) {
		spin_lock(&imgsensor_drv_lock);
		imgsensor.i2c_write_id = imgsensor_info.i2c_addr_table[i];
		spin_unlock(&imgsensor_drv_lock);
		do {
			reg_sensor_id = return_sensor_id();
			*sensor_id = imgsensor_convert_sensor_id(imgsensor_info.sensor_id,
					reg_sensor_id, vendorID_offset, 0);
			LOG_INF("sensor_module_product_id:0x%x.\n", *sensor_id);
			if (reg_sensor_id == tmp_sensor_id) {
				*sensor_id = imgsensor_info.sensor_id;
				LOG_INF("id reg: 0x%x, read id: 0x%x, expect id:0x%x.\n",
					imgsensor.i2c_write_id, *sensor_id, imgsensor_info.sensor_id);
				return ERROR_NONE;
			}
			LOG_INF("Check sensor id fail, id reg: 0x%x,read id: 0x%x, expect id:0x%x.\n",
				imgsensor.i2c_write_id, *sensor_id, imgsensor_info.sensor_id);
			retry--;
		} while (retry > 0);
		i++;
		retry = RETRY_TIMES;
	}
	if (*sensor_id != imgsensor_info.sensor_id) {
		/* if Sensor ID is not correct, Must set *sensor_id to 0xFFFFFFFF */
		*sensor_id = 0xFFFFFFFF;
		return ERROR_SENSOR_CONNECT_FAIL;
	}
	return ERROR_NONE;
}

/*************************************************************************
* FUNCTION
* 	open
*
* DESCRIPTION
* 	This function initialize the registers of CMOS sensor
*
* PARAMETERS
* 	None
*
* RETURNS
* 	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint32 open(void)
{
	int rc = 0;
	UINT32 sensor_id = 0;
	LOG_INF("ENTER\n");

	rc = get_imgsensor_id(&sensor_id);
	if (rc != ERROR_NONE)
		return ERROR_SENSOR_CONNECT_FAIL;
	LOG_DBG("sensor probe successfully. sensor_id=0x%x.\n", sensor_id);

	sensor_init();

	spin_lock(&imgsensor_drv_lock);

	imgsensor.autoflicker_en = KAL_FALSE;
	imgsensor.sensor_mode = IMGSENSOR_MODE_INIT;
	imgsensor.pclk = imgsensor_info.pre.pclk;
	imgsensor.frame_length = imgsensor_info.pre.framelength;
	imgsensor.line_length = imgsensor_info.pre.linelength;
	imgsensor.min_frame_length = imgsensor_info.pre.framelength;
	imgsensor.dummy_pixel = 0;
	imgsensor.dummy_line = 0;
	imgsensor.ihdr_en = KAL_FALSE;
	imgsensor.test_pattern = KAL_FALSE;
	imgsensor.current_fps = imgsensor_info.pre.max_framerate;
	spin_unlock(&imgsensor_drv_lock);
	LOG_INF("EXIT\n");

	return ERROR_NONE;
} /*  open  */

/*************************************************************************
* FUNCTION
* 	close
*
* DESCRIPTION
*
*
* PARAMETERS
* 	None
*
* RETURNS
* 	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint32 close(void)
{
	LOG_INF("Enter.\n");
	/* No Need to implement this function */

	return ERROR_NONE;
} /* 	close  */

/*************************************************************************
* FUNCTION
* preview
*
* DESCRIPTION
*   This function start the sensor preview.
*
* PARAMETERS
*   *image_window : address pointer of pixel numbers in one period of HSYNC
*  *sensor_config_data : address pointer of line numbers in one period of VSYNC
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint32 preview(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
			  MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	LOG_INF("ENTER\n");

	spin_lock(&imgsensor_drv_lock);
	imgsensor.sensor_mode = IMGSENSOR_MODE_PREVIEW;
	imgsensor.pclk = imgsensor_info.pre.pclk;
	imgsensor.line_length = imgsensor_info.pre.linelength;
	imgsensor.frame_length = imgsensor_info.pre.framelength;
	imgsensor.min_frame_length = imgsensor_info.pre.framelength;
	imgsensor.current_fps = imgsensor_info.pre.max_framerate;
	imgsensor.autoflicker_en = KAL_FALSE;
	spin_unlock(&imgsensor_drv_lock);
	set_mirror_flip(imgsensor.mirror);
	set_preview_setting();
	LOG_INF("EXIT\n");

	return ERROR_NONE;
} /*  preview   */

/*************************************************************************
* FUNCTION
*   capture
*
* DESCRIPTION
*   This function setup the CMOS sensor in capture MY_OUTPUT mode
*
* PARAMETERS
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
static kal_uint32 capture(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
			  MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	LOG_INF("ENTER\n");

	spin_lock(&imgsensor_drv_lock);
	imgsensor.sensor_mode = IMGSENSOR_MODE_CAPTURE;
	imgsensor.pclk = imgsensor_info.cap.pclk;
	imgsensor.line_length = imgsensor_info.cap.linelength;
	imgsensor.frame_length = imgsensor_info.cap.framelength;
	imgsensor.min_frame_length = imgsensor_info.cap.framelength;
	imgsensor.autoflicker_en = KAL_FALSE;
	spin_unlock(&imgsensor_drv_lock);

	set_mirror_flip(imgsensor.mirror);
	set_capture_setting();
	LOG_INF("EXIT\n");

	return ERROR_NONE;
} /* capture() */

static kal_uint32 normal_video(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
			       MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	LOG_INF("ENTER\n");

	spin_lock(&imgsensor_drv_lock);
	imgsensor.sensor_mode = IMGSENSOR_MODE_VIDEO;
	imgsensor.pclk = imgsensor_info.normal_video.pclk;
	imgsensor.line_length = imgsensor_info.normal_video.linelength;
	imgsensor.frame_length = imgsensor_info.normal_video.framelength;
	imgsensor.min_frame_length = imgsensor_info.normal_video.framelength;
	imgsensor.current_fps = imgsensor_info.normal_video.max_framerate;
	imgsensor.autoflicker_en = KAL_FALSE;
	spin_unlock(&imgsensor_drv_lock);

	set_mirror_flip(imgsensor.mirror);
	set_normal_video_setting();

	LOG_INF("EXIT\n");

	return ERROR_NONE;
} /*  normal_video   */

static kal_uint32 get_resolution(MSDK_SENSOR_RESOLUTION_INFO_STRUCT *sensor_resolution)
{
	if (sensor_resolution != NULL) {
		sensor_resolution->SensorFullWidth = imgsensor_info.cap.grabwindow_width;
		sensor_resolution->SensorFullHeight = imgsensor_info.cap.grabwindow_height;

		sensor_resolution->SensorPreviewWidth = imgsensor_info.pre.grabwindow_width;
		sensor_resolution->SensorPreviewHeight = imgsensor_info.pre.grabwindow_height;

		sensor_resolution->SensorVideoWidth = imgsensor_info.normal_video.grabwindow_width;
		sensor_resolution->SensorVideoHeight = imgsensor_info.normal_video.grabwindow_height;
	}
	return ERROR_NONE;
} /*    get_resolution	 */

static kal_uint32 get_info(enum MSDK_SCENARIO_ID_ENUM scenario_id,
			   MSDK_SENSOR_INFO_STRUCT *sensor_info,
			   MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	if (sensor_info != NULL && sensor_config_data != NULL) {
		sensor_info->SensorClockPolarity = SENSOR_CLOCK_POLARITY_LOW;
		sensor_info->SensorClockFallingPolarity = SENSOR_CLOCK_POLARITY_LOW; /* not use */
		sensor_info->SensorHsyncPolarity = SENSOR_CLOCK_POLARITY_LOW;  // inverse with datasheet
		sensor_info->SensorVsyncPolarity = SENSOR_CLOCK_POLARITY_LOW;
		sensor_info->SensorInterruptDelayLines = 4; /* not use */
		sensor_info->SensorResetActiveHigh = FALSE; /* not use */
		sensor_info->SensorResetDelayCount = 5; /* not use */

		sensor_info->SensroInterfaceType = imgsensor_info.sensor_interface_type;
		sensor_info->MIPIsensorType = imgsensor_info.mipi_sensor_type;
		sensor_info->SettleDelayMode = imgsensor_info.mipi_settle_delay_mode;
		sensor_info->SensorOutputDataFormat = imgsensor_info.sensor_output_dataformat;

		sensor_info->CaptureDelayFrame = imgsensor_info.cap_delay_frame;
		sensor_info->PreviewDelayFrame = imgsensor_info.pre_delay_frame;
		sensor_info->VideoDelayFrame = imgsensor_info.video_delay_frame;

		sensor_info->SensorMasterClockSwitch = 0; /* not use */
		sensor_info->SensorDrivingCurrent = imgsensor_info.isp_driving_current;

		sensor_info->AEShutDelayFrame =
			imgsensor_info.ae_shut_delay_frame; /* The frame of setting shutter default 0 for TG int */
		sensor_info->AESensorGainDelayFrame = imgsensor_info.ae_sensor_gain_delay_frame; /* The frame of setting sensor gain */
		sensor_info->AEISPGainDelayFrame = imgsensor_info.ae_ispGain_delay_frame;
		sensor_info->IHDR_Support = imgsensor_info.ihdr_support;
		sensor_info->IHDR_LE_FirstLine = imgsensor_info.ihdr_le_firstline;
		sensor_info->SensorModeNum = imgsensor_info.sensor_mode_num;

		sensor_info->SensorMIPILaneNumber = imgsensor_info.mipi_lane_num;
		sensor_info->SensorClockFreq = imgsensor_info.mclk;
		sensor_info->SensorClockDividCount = 3; /* not use */
		sensor_info->SensorClockRisingCount = 0;
		sensor_info->SensorClockFallingCount = 2; /* not use */
		sensor_info->SensorPixelClockCount = 3; /* not use */
		sensor_info->SensorDataLatchCount = 2; /* not use */

		sensor_info->MIPIDataLowPwr2HighSpeedTermDelayCount = 0;
		sensor_info->MIPICLKLowPwr2HighSpeedTermDelayCount = 0;
		sensor_info->SensorWidthSampling = 0;  // 0 is default 1x
		sensor_info->SensorHightSampling = 0;  // 0 is default 1x
		sensor_info->SensorPacketECCOrder = 1;

		switch (scenario_id) {
		case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
			sensor_info->SensorGrabStartX = imgsensor_info.pre.startx;
			sensor_info->SensorGrabStartY = imgsensor_info.pre.starty;
			sensor_info->MIPIDataLowPwr2HighSpeedSettleDelayCount = imgsensor_info.pre.mipi_data_lp2hs_settle_dc;
			break;
		case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
			sensor_info->SensorGrabStartX = imgsensor_info.cap.startx;
			sensor_info->SensorGrabStartY = imgsensor_info.cap.starty;
			sensor_info->MIPIDataLowPwr2HighSpeedSettleDelayCount = imgsensor_info.cap.mipi_data_lp2hs_settle_dc;
			break;
		case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
			sensor_info->SensorGrabStartX = imgsensor_info.normal_video.startx;
			sensor_info->SensorGrabStartY = imgsensor_info.normal_video.starty;
			sensor_info->MIPIDataLowPwr2HighSpeedSettleDelayCount = imgsensor_info.normal_video.mipi_data_lp2hs_settle_dc;
			break;
		default:
			sensor_info->SensorGrabStartX = imgsensor_info.pre.startx;
			sensor_info->SensorGrabStartY = imgsensor_info.pre.starty;
			sensor_info->MIPIDataLowPwr2HighSpeedSettleDelayCount = imgsensor_info.pre.mipi_data_lp2hs_settle_dc;
			break;
		}
	}
	return ERROR_NONE;
} /*    get_info  */

static kal_uint32 control(enum MSDK_SCENARIO_ID_ENUM scenario_id, MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
			  MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	LOG_DBG("scenario_id = %d\n", scenario_id);
	spin_lock(&imgsensor_drv_lock);
	imgsensor.current_scenario_id = scenario_id;
	spin_unlock(&imgsensor_drv_lock);
	switch (scenario_id) {
	case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
		preview(image_window, sensor_config_data);
		break;
	case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
		capture(image_window, sensor_config_data);
		break;
	case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
		normal_video(image_window, sensor_config_data);
		break;
	default:
		LOG_ERR("Error ScenarioId setting");
		preview(image_window, sensor_config_data);
		return ERROR_INVALID_SCENARIO_ID;
	}
	return ERROR_NONE;
} /* control() */

static kal_uint32 set_video_mode(UINT16 framerate)
{
	LOG_INF("framerate = %d\n ", framerate);
	// SetVideoMode Function should fix framerate
	if (framerate == 0)
		// Dynamic frame rate
		return ERROR_NONE;
	spin_lock(&imgsensor_drv_lock);
	/* fps set to 298 when frame is 300 and auto-flicker enaled */
	if ((framerate == 300) && (imgsensor.autoflicker_en == KAL_TRUE))
		imgsensor.current_fps = 298;
	/* fps set to 146 when frame is 150 and auto-flicker enaled */
	else if ((framerate == 150) && (imgsensor.autoflicker_en == KAL_TRUE))
		imgsensor.current_fps = 146;
	else
		imgsensor.current_fps = framerate;
	spin_unlock(&imgsensor_drv_lock);
	set_max_framerate(imgsensor.current_fps, 1);

	return ERROR_NONE;
}

static kal_uint32 set_auto_flicker_mode(kal_bool enable, UINT16 framerate)
{
	LOG_INF("enable = %d, framerate = %d \n", enable, framerate);
	spin_lock(&imgsensor_drv_lock);
	if (enable)   // enable auto flicker
		imgsensor.autoflicker_en = KAL_TRUE;
	else   // Cancel Auto flick
		imgsensor.autoflicker_en = KAL_FALSE;
	spin_unlock(&imgsensor_drv_lock);
	return ERROR_NONE;
}

static kal_uint32 set_max_framerate_by_scenario(enum MSDK_SCENARIO_ID_ENUM scenario_id, UINT32 framerate)
{
	kal_uint32 frame_length = 0;

	LOG_DBG("scenario_id = %d, framerate = %d\n", scenario_id, framerate);
	switch (scenario_id) {
	case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
		if (framerate == 0 || imgsensor_info.pre.linelength == 0)
			return ERROR_NONE;
		frame_length = imgsensor_info.pre.pclk / framerate * 10 / imgsensor_info.pre.linelength;

		spin_lock(&imgsensor_drv_lock);
		imgsensor.dummy_line = (frame_length > imgsensor_info.pre.framelength) ? (frame_length - imgsensor_info.pre.framelength) : 0;
		imgsensor.frame_length = imgsensor_info.pre.framelength + imgsensor.dummy_line;
		imgsensor.min_frame_length = imgsensor.frame_length;
		spin_unlock(&imgsensor_drv_lock);
		break;
	case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
		if (framerate == 0 || imgsensor_info.normal_video.linelength == 0)
			return ERROR_NONE;
		frame_length = imgsensor_info.normal_video.pclk / framerate * 10 / imgsensor_info.normal_video.linelength;
		spin_lock(&imgsensor_drv_lock);
		imgsensor.dummy_line = (frame_length > imgsensor_info.normal_video.framelength) ? (frame_length -
				       imgsensor_info.normal_video.framelength) : 0;
		imgsensor.frame_length = imgsensor_info.normal_video.framelength + imgsensor.dummy_line;
		imgsensor.min_frame_length = imgsensor.frame_length;
		spin_unlock(&imgsensor_drv_lock);
		break;
	case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
		if (framerate == 0 || imgsensor_info.cap.linelength == 0)
			return ERROR_NONE;
		frame_length = imgsensor_info.cap.pclk / framerate * 10 / imgsensor_info.cap.linelength;
		spin_lock(&imgsensor_drv_lock);
		imgsensor.dummy_line = (frame_length > imgsensor_info.cap.framelength) ? (frame_length - imgsensor_info.cap.framelength) : 0;
		imgsensor.frame_length = imgsensor_info.cap.framelength + imgsensor.dummy_line;
		imgsensor.min_frame_length = imgsensor.frame_length;
		spin_unlock(&imgsensor_drv_lock);
		break;
	default:  // coding with  preview scenario by default
		if (framerate == 0 || imgsensor_info.pre.linelength == 0)
			return ERROR_NONE;
		frame_length = imgsensor_info.pre.pclk / framerate * 10 / imgsensor_info.pre.linelength;
		spin_lock(&imgsensor_drv_lock);
		imgsensor.dummy_line = (frame_length > imgsensor_info.pre.framelength) ? (frame_length - imgsensor_info.pre.framelength) : 0;
		imgsensor.frame_length = imgsensor_info.pre.framelength + imgsensor.dummy_line;
		imgsensor.min_frame_length = imgsensor.frame_length;
		spin_unlock(&imgsensor_drv_lock);
		LOG_ERR("error scenario_id = %d, we use preview scenario \n", scenario_id);
		break;
	}
	return ERROR_NONE;
}

static kal_uint32 get_default_framerate_by_scenario(enum MSDK_SCENARIO_ID_ENUM scenario_id, UINT32 *framerate)
{
	if (framerate != NULL) {
		switch (scenario_id) {
		case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
			*framerate = imgsensor_info.pre.max_framerate;
			break;
		case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
			*framerate = imgsensor_info.normal_video.max_framerate;
			break;
		case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
			*framerate = imgsensor_info.cap.max_framerate;
			break;
		default:
			break;
		}
	}
	return ERROR_NONE;
}

static kal_uint32 set_test_pattern_mode(kal_bool enable)
{
	/*
	0x0A04(SENSOR_ISP_EN_REG_H)
	bit[7:1]: Reserved
	bit[0]: mipi enable

	0x0A05(SENSOR_ISP_EN_REG_L)
	bit[7]: Reserved
	bit[6]: Fmt enable
	bit[5]: H binning enable
	bit[4]: Lsc enable
	bit[3]: dga enable
	bit[2]: Reserved
	bit[1]: adpc enable
	bit[0]: tpg enable
	*/
	/*
	0x020a(SENSOR_TP_MODE_REG)
	bit[3:0]: TP Mode
	0: no pattern
	1: solid color
	2: 100% color bars
	3: Fade to grey color bars
	4: PN9
	5: h gradient pattern
	6: v gradient pattern
	7: check board
	8: slant pattern
	9: resolution pattern
	*/
	if (enable) {
		LOG_INF("enter color bar");
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_ISP_EN_REG_H, 0x0141, IMGSENSOR_I2C_WORD_DATA);
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_TP_MODE_REG, 0x0200, IMGSENSOR_I2C_WORD_DATA);
	} else {
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_ISP_EN_REG_H, 0x0142, IMGSENSOR_I2C_WORD_DATA);
		(void)imgsensor_sensor_i2c_write(&imgsensor, SENSOR_TP_MODE_REG, 0x0000, IMGSENSOR_I2C_WORD_DATA);
	}
	spin_lock(&imgsensor_drv_lock);
	imgsensor.test_pattern = enable;
	spin_unlock(&imgsensor_drv_lock);
	return ERROR_NONE;
}

static kal_uint32 streaming_control(kal_bool enable)
{
	kal_int32 rc = 0;

	LOG_INF("Enter.enable:%d\n", enable);
	if (enable)
		rc = imgsensor_sensor_write_setting(&imgsensor, &imgsensor_info.streamon_setting);
	else
		rc = imgsensor_sensor_write_setting(&imgsensor, &imgsensor_info.streamoff_setting);
	if (rc < 0) {
		LOG_ERR("Failed enable:%d.\n", enable);
		return ERROR_SENSOR_POWER_ON_FAIL;
	}
	LOG_INF("Exit.enable:%d\n", enable);

	return ERROR_NONE;
}

static kal_uint32 feature_control_hi846_byd(MSDK_SENSOR_FEATURE_ENUM feature_id,
		UINT8 *feature_para, UINT32 *feature_para_len)
{
	if (feature_para != NULL && feature_para_len != NULL) {
		UINT16 *feature_return_para_16 = (UINT16 *)feature_para;
		UINT16 *feature_data_16 = (UINT16 *)feature_para;
		UINT32 *feature_return_para_32 = (UINT32 *)feature_para;
		UINT32 *feature_data_32 = (UINT32 *)feature_para;
		unsigned long long *feature_data = (unsigned long long *)feature_para;

		struct SENSOR_WINSIZE_INFO_STRUCT *wininfo;

		LOG_DBG("feature_id = %d.\n", feature_id);
		switch (feature_id) {
		case SENSOR_FEATURE_GET_PERIOD:
			*feature_return_para_16++ = imgsensor.line_length;
			*feature_return_para_16 = imgsensor.frame_length;
			*feature_para_len = 4;
			break;
		case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
			*feature_return_para_32 = imgsensor.pclk;
			*feature_para_len = 4;
			break;
		case SENSOR_FEATURE_SET_ESHUTTER:
			set_shutter((UINT16)*feature_data);
			break;
		case SENSOR_FEATURE_SET_GAIN:
			set_gain((UINT16)*feature_data);
			break;
		case SENSOR_FEATURE_SET_VIDEO_MODE:
			set_video_mode((UINT16)*feature_data);
			break;
		case SENSOR_FEATURE_CHECK_SENSOR_ID:
			get_imgsensor_id(feature_return_para_32);
			break;
		case SENSOR_FEATURE_SET_AUTO_FLICKER_MODE:
			set_auto_flicker_mode((BOOL)*feature_data_16, *(feature_data_16 + 1));
			break;
		case SENSOR_FEATURE_SET_MAX_FRAME_RATE_BY_SCENARIO:
			set_max_framerate_by_scenario((enum MSDK_SCENARIO_ID_ENUM) * feature_data, (UINT32) * (feature_data + 1));
			break;
		case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
			get_default_framerate_by_scenario((enum MSDK_SCENARIO_ID_ENUM) * (feature_data),
							  (UINT32 *)(uintptr_t)(*(feature_data + 1)));
			break;
		case SENSOR_FEATURE_SET_TEST_PATTERN:
			set_test_pattern_mode((BOOL)*feature_data);
			break;
		case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE:  // for factory mode auto testing
			*feature_return_para_32 = imgsensor_info.checksum_value;
			*feature_para_len = 4;
			break;
		case SENSOR_FEATURE_SET_FRAMERATE:
			LOG_DBG("current fps :%d\n", *feature_data_32);
			spin_lock(&imgsensor_drv_lock);
			imgsensor.current_fps = (UINT16) * feature_data_32;
			spin_unlock(&imgsensor_drv_lock);
			break;
		case SENSOR_FEATURE_GET_CROP_INFO:
			wininfo = (struct SENSOR_WINSIZE_INFO_STRUCT *)(uintptr_t)(*(feature_data + 1));
			switch (*feature_data_32) {
			case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
				/* imgsensor_winsize_info arry 1 is capture setting */
				memcpy((void *)wininfo, (void *)&imgsensor_winsize_info[1], sizeof(struct SENSOR_WINSIZE_INFO_STRUCT));
				break;
			case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
				/* imgsensor_winsize_info arry 2 is preview setting */
				memcpy((void *)wininfo, (void *)&imgsensor_winsize_info[2], sizeof(struct SENSOR_WINSIZE_INFO_STRUCT));
				break;
			case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
			default:
				/* imgsensor_winsize_info arry 2 is preview setting */
				memcpy((void *)wininfo, (void *)&imgsensor_winsize_info[0], sizeof(struct SENSOR_WINSIZE_INFO_STRUCT));
				break;
			}
			break;
		case SENSOR_FEATURE_SET_STREAMING_SUSPEND:
			(void)streaming_control(KAL_FALSE);
			break;
		case SENSOR_FEATURE_SET_STREAMING_RESUME:
			if (*feature_data != 0)
				set_shutter((UINT16)*feature_data);
			(void)streaming_control(KAL_TRUE);
			break;
		case SENSOR_HUAWEI_FEATURE_DUMP_REG:
			sensor_dump_reg();
			break;
		case SENSOR_FEATURE_GET_MIPI_PIXEL_RATE:
			switch (*feature_data) {
			case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
				*(UINT32 *)(uintptr_t)(*(feature_data + 1)) =
					imgsensor_info.cap.mipi_pixel_rate;
				break;
			case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
				*(UINT32 *)(uintptr_t)(*(feature_data + 1)) =
					imgsensor_info.normal_video.mipi_pixel_rate;
				break;
			case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
			default:
				*(UINT32 *)(uintptr_t)(*(feature_data + 1)) =
					imgsensor_info.pre.mipi_pixel_rate;
				break;
			}
			break;
		case SENSOR_FEATURE_GET_PIXEL_RATE:
			switch (*feature_data) {
			case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
				if (imgsensor_info.cap.linelength > IMGSENSOR_LINGLENGTH_GAP) {
					*(UINT32 *)(uintptr_t)(*(feature_data + 1)) =
						(imgsensor_info.cap.pclk /
						 (imgsensor_info.cap.linelength - IMGSENSOR_LINGLENGTH_GAP)) *
						imgsensor_info.cap.grabwindow_width;
				}
				break;
			case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
				if (imgsensor_info.normal_video.linelength > IMGSENSOR_LINGLENGTH_GAP) {
					*(UINT32 *)(uintptr_t)(*(feature_data + 1)) =
						(imgsensor_info.normal_video.pclk /
						 (imgsensor_info.normal_video.linelength - IMGSENSOR_LINGLENGTH_GAP)) *
						imgsensor_info.normal_video.grabwindow_width;
				}
				break;
			case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
			default:
				if (imgsensor_info.pre.linelength > IMGSENSOR_LINGLENGTH_GAP) {
					*(UINT32 *)(uintptr_t)(*(feature_data + 1)) =
						(imgsensor_info.pre.pclk /
						 (imgsensor_info.pre.linelength - IMGSENSOR_LINGLENGTH_GAP)) *
						imgsensor_info.pre.grabwindow_width;
				}
				break;
			}
			break;
		default:
			LOG_INF("Not support the feature_id:%d\n", feature_id);
			break;
		}
	}
	return ERROR_NONE;
} /* feature_control() */

static struct SENSOR_FUNCTION_STRUCT sensor_func = {
	open,
	get_info,
	get_resolution,
	feature_control_hi846_byd,
	control,
	close
};

UINT32 HI846_BYD_SensorInit(struct SENSOR_FUNCTION_STRUCT **pfFunc)
{
	/* To Do : Check Sensor status here */
	if (pfFunc != NULL)
		*pfFunc = &sensor_func;
	return ERROR_NONE;
} /* 	HI846_QTECH_MIPI_RAW_SensorInit   */
