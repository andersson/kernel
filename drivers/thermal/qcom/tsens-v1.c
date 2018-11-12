// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Limited
 */

#include <linux/regmap.h>
#include <linux/bitops.h>
#include "tsens.h"

/* eeprom layout data for qcs404 (v1) */
#define BASE0_MASK	0x000007f8
#define BASE1_MASK	0x0007f800
#define BASE0_SHIFT	3
#define BASE1_SHIFT	11

#define S0_P1_MASK	0x0000003f
#define S1_P1_MASK	0x0003f000
#define S2_P1_MASK	0x3f000000
#define S3_P1_MASK	0x000003f0
#define S4_P1_MASK	0x003f0000
#define S5_P1_MASK	0x0000003f
#define S6_P1_MASK	0x0003f000
#define S7_P1_MASK	0x3f000000
#define S8_P1_MASK	0x000003f0
#define S9_P1_MASK	0x003f0000

#define S0_P2_MASK	0x00000fc0
#define S1_P2_MASK	0x00fc0000
#define S2_P2_MASK_1_0	0xc0000000
#define S2_P2_MASK_5_2	0x0000000f
#define S3_P2_MASK	0x0000fc00
#define S4_P2_MASK	0x0fc00000
#define S5_P2_MASK	0x00000fc0
#define S6_P2_MASK	0x00fc0000
#define S7_P2_MASK_1_0	0xc0000000
#define S7_P2_MASK_5_2	0x0000000f
#define S8_P2_MASK	0x0000fc00
#define S9_P2_MASK	0x0fc00000

#define S0_P1_SHIFT	0
#define S0_P2_SHIFT	6
#define S1_P1_SHIFT	12
#define S1_P2_SHIFT	18
#define S2_P1_SHIFT	24
#define S2_P2_SHIFT_1_0	30

#define S2_P2_SHIFT_5_2	0
#define S3_P1_SHIFT	4
#define S3_P2_SHIFT	10
#define S4_P1_SHIFT	16
#define S4_P2_SHIFT	22

#define S5_P1_SHIFT	0
#define S5_P2_SHIFT	6
#define S6_P1_SHIFT	12
#define S6_P2_SHIFT	18
#define S7_P1_SHIFT	24
#define S7_P2_SHIFT_1_0	30

#define S7_P2_SHIFT_5_2	0
#define S8_P1_SHIFT	4
#define S8_P2_SHIFT	10
#define S9_P1_SHIFT	16
#define S9_P2_SHIFT	22

#define CAL_SEL_MASK	7
#define CAL_SEL_SHIFT	0

static int calibrate_v1(struct tsens_device *tmdev)
{
	u32 base0 = 0, base1 = 0;
	u32 p1[tmdev->num_sensors], p2[tmdev->num_sensors];
	u32 mode = 0, lsb = 0, msb = 0;
	u32 *qfprom_cdata;
	int i;

	qfprom_cdata = (u32 *)qfprom_read(tmdev->dev, "calib");
	if (IS_ERR(qfprom_cdata))
		return PTR_ERR(qfprom_cdata);

	mode = (qfprom_cdata[4] & CAL_SEL_MASK) >> CAL_SEL_SHIFT;
	dev_dbg(tmdev->dev, "calibration mode is %d\n", mode);

	switch (mode) {
	case TWO_PT_CALIB:
		base1 = (qfprom_cdata[4] & BASE1_MASK) >> BASE1_SHIFT;
		p2[0] = (qfprom_cdata[0] & S0_P2_MASK) >> S0_P2_SHIFT;
		p2[1] = (qfprom_cdata[0] & S1_P2_MASK) >> S1_P2_SHIFT;
		/* This value is split over two registers, 2 bits and 4 bits */
		lsb   = (qfprom_cdata[0] & S2_P2_MASK_1_0) >> S2_P2_SHIFT_1_0;
		msb   = (qfprom_cdata[1] & S2_P2_MASK_5_2) >> S2_P2_SHIFT_5_2;
		p2[2] = msb << 2 | lsb;
		p2[3] = (qfprom_cdata[1] & S3_P2_MASK) >> S3_P2_SHIFT;
		p2[4] = (qfprom_cdata[1] & S4_P2_MASK) >> S4_P2_SHIFT;
		p2[5] = (qfprom_cdata[2] & S5_P2_MASK) >> S5_P2_SHIFT;
		p2[6] = (qfprom_cdata[2] & S6_P2_MASK) >> S6_P2_SHIFT;
		/* This value is split over two registers, 2 bits and 4 bits */
		lsb   = (qfprom_cdata[2] & S7_P2_MASK_1_0) >> S7_P2_SHIFT_1_0;
		msb   = (qfprom_cdata[3] & S7_P2_MASK_5_2) >> S7_P2_SHIFT_5_2;
		p2[7] = msb << 2 | lsb;
		p2[8] = (qfprom_cdata[3] & S8_P2_MASK) >> S8_P2_SHIFT;
		p2[9] = (qfprom_cdata[3] & S9_P2_MASK) >> S9_P2_SHIFT;
		for (i = 0; i < tmdev->num_sensors; i++)
			p2[i] = ((base1 + p2[i]) << 2);
		/* Fall through */
	case ONE_PT_CALIB2:
		base0 = (qfprom_cdata[4] & BASE0_MASK) >> BASE0_SHIFT;
		p1[0] = (qfprom_cdata[0] & S0_P1_MASK) >> S0_P1_SHIFT;
		p1[1] = (qfprom_cdata[0] & S1_P1_MASK) >> S1_P1_SHIFT;
		p1[2] = (qfprom_cdata[0] & S2_P1_MASK) >> S2_P1_SHIFT;
		p1[3] = (qfprom_cdata[1] & S3_P1_MASK) >> S3_P1_SHIFT;
		p1[4] = (qfprom_cdata[1] & S4_P1_MASK) >> S4_P1_SHIFT;
		p1[5] = (qfprom_cdata[2] & S5_P1_MASK) >> S5_P1_SHIFT;
		p1[6] = (qfprom_cdata[2] & S6_P1_MASK) >> S6_P1_SHIFT;
		p1[7] = (qfprom_cdata[2] & S7_P1_MASK) >> S7_P1_SHIFT;
		p1[8] = (qfprom_cdata[3] & S8_P1_MASK) >> S8_P1_SHIFT;
		p1[9] = (qfprom_cdata[3] & S9_P1_MASK) >> S9_P1_SHIFT;
		for (i = 0; i < tmdev->num_sensors; i++)
			p1[i] = (((base0) + p1[i]) << 2);
		break;
	default:
		for (i = 0; i < tmdev->num_sensors; i++) {
			p1[i] = 500;
			p2[i] = 780;
		}
		break;
	}

	compute_intercept_slope(tmdev, p1, p2, mode);

	return 0;
}

#define STATUS_OFFSET		0x44
#define LAST_TEMP_MASK		0x3ff
#define STATUS_VALID_BIT	BIT(14)

static int get_temp_tsens_v1(struct tsens_device *tmdev, int id, int *temp)
{
	struct tsens_sensor *s = &tmdev->sensor[id];
	u32 code;
	unsigned int status_reg;
	u32 last_temp = 0, last_temp2 = 0, last_temp3 = 0;
	int ret;

	status_reg = tmdev->tm_offset + STATUS_OFFSET + s->hw_id * 4;
	ret = regmap_read(tmdev->tm_map, status_reg, &code);
	if (ret)
		return ret;
	last_temp = code & LAST_TEMP_MASK;
	if (code & STATUS_VALID_BIT)
		goto done;

	/* Try a second time */
	ret = regmap_read(tmdev->tm_map, status_reg, &code);
	if (ret)
		return ret;
	if (code & STATUS_VALID_BIT) {
		last_temp = code & LAST_TEMP_MASK;
		goto done;
	} else {
		last_temp2 = code & LAST_TEMP_MASK;
	}

	/* Try a third/last time */
	ret = regmap_read(tmdev->tm_map, status_reg, &code);
	if (ret)
		return ret;
	if (code & STATUS_VALID_BIT) {
		last_temp = code & LAST_TEMP_MASK;
		goto done;
	} else {
		last_temp3 = code & LAST_TEMP_MASK;
	}

	if (last_temp == last_temp2)
		last_temp = last_temp2;
	else if (last_temp2 == last_temp3)
		last_temp = last_temp3;
done:
	/* Convert temperature from deciCelsius to milliCelsius */
	*temp = sign_extend32(last_temp, fls(LAST_TEMP_MASK) - 1) * 100;

	return 0;
}

static const struct tsens_ops ops_generic_v1 = {
	.init		= init_common,
	.calibrate	= calibrate_v1,
	.get_temp	= get_temp_tsens_v1,
};

const struct tsens_data data_tsens_v1 = {
	.ops		= &ops_generic_v1,
	.reg_offsets	= { [SROT_CTRL_OFFSET] = 0x4 },
};
