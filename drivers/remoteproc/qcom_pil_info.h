#ifndef __QCOM_PIL_INFO_H__
#define __QCOM_PIL_INFO_H__

void qcom_pil_info_store(const char *image, phys_addr_t base, size_t size);
bool qcom_pil_info_available(void);

#endif
