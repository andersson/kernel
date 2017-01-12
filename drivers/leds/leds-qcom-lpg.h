#ifndef __LEDS_QCOM_LPG_H__
#define __LEDS_QCOM_LPG_H__

struct qcom_tri_led;
struct qcom_lpg_lut;

/*
 * qcom_lpg_pattern - object tracking allocated LUT entries
 * @lut:	reference to the client & LUT device context
 * @lo_idx:	index of first entry in the LUT used by pattern
 * @hi_idx:	index of the last entry in the LUT used by pattern
 */
struct qcom_lpg_pattern {
	struct qcom_lpg_lut *lut;

	unsigned int lo_idx;
	unsigned int hi_idx;
};

struct qcom_tri_led *qcom_tri_led_get(struct device *dev);
int qcom_tri_led_set(struct qcom_tri_led *tri, bool enabled);

struct qcom_lpg_lut *qcom_lpg_lut_get(struct device *dev);
struct qcom_lpg_pattern *qcom_lpg_lut_store(struct qcom_lpg_lut *lut,
					    const u16 *values, size_t len);
u16 *qcom_lpg_lut_read(struct qcom_lpg_pattern *pattern, size_t *len);
void qcom_lpg_lut_free(struct qcom_lpg_pattern *pattern);
int qcom_lpg_lut_sync(struct qcom_lpg_lut *lut);

#endif
