// SPDX-License-Identifier: GPL-2.0

#ifndef __Q6DSP_ERR_NO_H__
#define __Q6DSP_ERR_NO_H__
#include <linux/kernel.h>

/* Success. The operation completed with no errors. */
#define ADSP_EOK          0x00000000
/* General failure. */
#define ADSP_EFAILED      0x00000001
/* Bad operation parameter. */
#define ADSP_EBADPARAM    0x00000002
/* Unsupported routine or operation. */
#define ADSP_EUNSUPPORTED 0x00000003
/* Unsupported version. */
#define ADSP_EVERSION     0x00000004
/* Unexpected problem encountered. */
#define ADSP_EUNEXPECTED  0x00000005
/* Unhandled problem occurred. */
#define ADSP_EPANIC       0x00000006
/* Unable to allocate resource. */
#define ADSP_ENORESOURCE  0x00000007
/* Invalid handle. */
#define ADSP_EHANDLE      0x00000008
/* Operation is already processed. */
#define ADSP_EALREADY     0x00000009
/* Operation is not ready to be processed. */
#define ADSP_ENOTREADY    0x0000000A
/* Operation is pending completion. */
#define ADSP_EPENDING     0x0000000B
/* Operation could not be accepted or processed. */
#define ADSP_EBUSY        0x0000000C
/* Operation aborted due to an error. */
#define ADSP_EABORTED     0x0000000D
/* Operation preempted by a higher priority. */
#define ADSP_EPREEMPTED   0x0000000E
/* Operation requests intervention to complete. */
#define ADSP_ECONTINUE    0x0000000F
/* Operation requests immediate intervention to complete. */
#define ADSP_EIMMEDIATE   0x00000010
/* Operation is not implemented. */
#define ADSP_ENOTIMPL     0x00000011
/* Operation needs more data or resources. */
#define ADSP_ENEEDMORE    0x00000012
/* Operation does not have memory. */
#define ADSP_ENOMEMORY    0x00000014
/* Item does not exist. */
#define ADSP_ENOTEXIST    0x00000015
/* Max count for adsp error code sent to HLOS*/

struct q6dsp_err_code {
	int	lnx_err_code;
	char	*adsp_err_str;
};

static struct q6dsp_err_code q6dsp_err_codes[] = {
	[ADSP_EFAILED] = { -ENOTRECOVERABLE, "ADSP_EFAILED"},
	[ADSP_EBADPARAM] = { -EINVAL, "ADSP_EBADPARAM"},
	[ADSP_EUNSUPPORTED] = { -ENOSYS, "ADSP_EUNSUPPORTED"},
	[ADSP_EVERSION] = { -ENOPROTOOPT, "ADSP_EVERSION"},
	[ADSP_EUNEXPECTED] = { -ENOTRECOVERABLE, "ADSP_EUNEXPECTED"},
	[ADSP_EPANIC] = { -ENOTRECOVERABLE, "ADSP_EPANIC"},
	[ADSP_ENORESOURCE] = { -ENOSPC, "ADSP_ENORESOURCE"},
	[ADSP_EHANDLE] = { -EBADR, "ADSP_EHANDLE"},
	[ADSP_EALREADY] = { -EALREADY, "ADSP_EALREADY"},
	[ADSP_ENOTREADY] = { -EPERM, "ADSP_ENOTREADY"},
	[ADSP_EPENDING] = { -EINPROGRESS, "ADSP_EPENDING"},
	[ADSP_EBUSY] = { -EBUSY, "ADSP_EBUSY"},
	[ADSP_EABORTED] = { -ECANCELED, "ADSP_EABORTED"},
	[ADSP_EPREEMPTED] = { -EAGAIN, "ADSP_EPREEMPTED"},
	[ADSP_ECONTINUE] = { -EAGAIN, "ADSP_ECONTINUE"},
	[ADSP_EIMMEDIATE] = { -EAGAIN, "ADSP_EIMMEDIATE"},
	[ADSP_ENOTIMPL] = { -EAGAIN, "ADSP_ENOTIMPL"},
	[ADSP_ENEEDMORE] = { -ENODATA, "ADSP_ENEEDMORE"},
	[ADSP_ENOMEMORY] = { -EINVAL, "ADSP_ENOMEMORY"},
	[ADSP_ENOTEXIST] = { -ENOENT, "ADSP_ENOTEXIST"},
};

static inline int q6dsp_errno(u32 error)
{
	int ret = -EINVAL;

	if (error <= ARRAY_SIZE(q6dsp_err_codes))
		ret = q6dsp_err_codes[error].lnx_err_code;

	return ret;
}

static inline char *q6dsp_strerror(u32 error)
{
	if (error <= ARRAY_SIZE(q6dsp_err_codes))
		return q6dsp_err_codes[error].adsp_err_str;

	return  "ADSP_ERR_MAX";
}

#endif /*__Q6DSP_ERR_NO_H__ */
