/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/qcom_scm.h>
#include <linux/arm-smccc.h>
#include <linux/dma-mapping.h>

#include "qcom_scm.h"

#define QCOM_SCM_FNID(s, c) ((((s) & 0xFF) << 8) | ((c) & 0xFF))

#define MAX_QCOM_SCM_ARGS 10
#define MAX_QCOM_SCM_RETS 3

enum qcom_scm_arg_types {
	QCOM_SCM_VAL,
	QCOM_SCM_RO,
	QCOM_SCM_RW,
	QCOM_SCM_BUFVAL,
};

#define QCOM_SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) (\
			   (((a) & 0x3) << 4) | \
			   (((b) & 0x3) << 6) | \
			   (((c) & 0x3) << 8) | \
			   (((d) & 0x3) << 10) | \
			   (((e) & 0x3) << 12) | \
			   (((f) & 0x3) << 14) | \
			   (((g) & 0x3) << 16) | \
			   (((h) & 0x3) << 18) | \
			   (((i) & 0x3) << 20) | \
			   (((j) & 0x3) << 22) | \
			   ((num) & 0xf))

#define QCOM_SCM_ARGS(...) QCOM_SCM_ARGS_IMPL(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

/**
 * struct qcom_scm_desc
 * @arginfo:	Metadata describing the arguments in args[]
 * @args:	The array of arguments for the secure syscall
 * @res:	The values returned by the secure syscall
 */
struct qcom_scm_desc {
	u32 arginfo;
	u64 args[MAX_QCOM_SCM_ARGS];
};

static u64 qcom_smccc_convention = -1;
static DEFINE_MUTEX(qcom_scm_lock);

#define QCOM_SCM_EBUSY_WAIT_MS 30
#define QCOM_SCM_EBUSY_MAX_RETRY 20

#define N_EXT_QCOM_SCM_ARGS 7
#define FIRST_EXT_ARG_IDX 3
#define N_REGISTER_ARGS (MAX_QCOM_SCM_ARGS - N_EXT_QCOM_SCM_ARGS + 1)

#if 1

#define __asmeq(x, y)  ".ifnc " x "," y " ; .err ; .endif\n\t"

#define R0_STR "x0"
#define R1_STR "x1"
#define R2_STR "x2"
#define R3_STR "x3"
#define R4_STR "x4"
#define R5_STR "x5"
#define R6_STR "x6"

static int __qcom_scm_call_armv8_32(u32 w0, u32 w1, u32 w2, u32 w3, u32 w4, u32 w5,
				u64 *ret1, u64 *ret2, u64 *ret3)
{
	register u32 r0 asm("r0") = w0;
	register u32 r1 asm("r1") = w1;
	register u32 r2 asm("r2") = w2;
	register u32 r3 asm("r3") = w3;
	register u32 r4 asm("r4") = w4;
	register u32 r5 asm("r5") = w5;
	register u32 r6 asm("r6") = 0;

	do {
		asm volatile(
			__asmeq("%0", R0_STR)
			__asmeq("%1", R1_STR)
			__asmeq("%2", R2_STR)
			__asmeq("%3", R3_STR)
			__asmeq("%4", R0_STR)
			__asmeq("%5", R1_STR)
			__asmeq("%6", R2_STR)
			__asmeq("%7", R3_STR)
			__asmeq("%8", R4_STR)
			__asmeq("%9", R5_STR)
			__asmeq("%10", R6_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
			"smc	#0\n"
			: "=r" (r0), "=r" (r1), "=r" (r2), "=r" (r3)
			: "r" (r0), "r" (r1), "r" (r2), "r" (r3), "r" (r4),
			  "r" (r5), "r" (r6)
			: "x7", "x8", "x9", "x10", "x11", "x12", "x13",
			"x14", "x15", "x16", "x17");

	} while (r0 == QCOM_SCM_INTERRUPTED);

	if (ret1)
		*ret1 = r1;
	if (ret2)
		*ret2 = r2;
	if (ret3)
		*ret3 = r3;

	return r0;
}

#define QCOM_SCM_SIP_FNID(s, c) (((((s) & 0xFF) << 8) | ((c) & 0xFF)) | 0x02000000)

static int qcom_scm_call(struct device *dev, u32 svc_id, u32 cmd_id,
			 const struct qcom_scm_desc *desc,
			 struct arm_smccc_res *res)
{
	int ret, retry_count = 0;
	u32 fn_id = QCOM_SCM_SIP_FNID(svc_id, cmd_id);
	u64 x0;
	u64 ret1, ret2, ret3;

	x0 = fn_id;

	do {
		mutex_lock(&qcom_scm_lock);

		ret = ret1 = ret2 = ret3 = 0;

		ret = __qcom_scm_call_armv8_32(x0, desc->arginfo,
						desc->args[0], desc->args[1],
						desc->args[2], desc->args[3],
						&ret1, &ret2, &ret3);
		mutex_unlock(&qcom_scm_lock);

		if (ret == QCOM_SCM_V2_EBUSY)
			msleep(QCOM_SCM_EBUSY_WAIT_MS);

	}  while (ret == QCOM_SCM_V2_EBUSY && (retry_count++ < QCOM_SCM_EBUSY_MAX_RETRY));

	res->a0 = (unsigned long)ret;
	res->a1 = ret1;
	res->a2 = ret2;
	res->a3 = ret3;

	if (ret < 0)
		pr_err("%s: error: funcid %llx, arginfo: %x, "
			"args: %llx, %llx, %llx, %llx, "
			"syscall returns: %lx, %llx, %llx, %llx"
			" (%d)\n", __func__,
			x0, desc->arginfo,
			desc->args[0], desc->args[1], desc->args[2], desc->args[3],
			res->a0, ret1, ret2, ret3, ret);

	if (ret < 0)
		return qcom_scm_remap_error(ret);

	return 0;

}

#else
/**
 * qcom_scm_call() - Invoke a syscall in the secure world
 * @dev:	device
 * @svc_id:	service identifier
 * @cmd_id:	command identifier
 * @desc:	Descriptor structure containing arguments and return values
 *
 * Sends a command to the SCM and waits for the command to finish processing.
 * This should *only* be called in pre-emptible context.
*/
static int qcom_scm_call(struct device *dev, u32 svc_id, u32 cmd_id,
			 const struct qcom_scm_desc *desc,
			 struct arm_smccc_res *res)
{
	int arglen = desc->arginfo & 0xf;
	int retry_count = 0, i;
	u32 fn_id = QCOM_SCM_FNID(svc_id, cmd_id);
	u64 cmd, x5 = desc->args[FIRST_EXT_ARG_IDX];
	dma_addr_t args_phys = 0;
	void *args_virt = NULL;
	size_t alloc_len;

	if (unlikely(arglen > N_REGISTER_ARGS)) {
		alloc_len = N_EXT_QCOM_SCM_ARGS * sizeof(u64);
		args_virt = kzalloc(PAGE_ALIGN(alloc_len), GFP_KERNEL);

		if (!args_virt)
			return -ENOMEM;

		if (qcom_smccc_convention == ARM_SMCCC_SMC_32) {
			__le32 *args = args_virt;

			for (i = 0; i < N_EXT_QCOM_SCM_ARGS; i++)
				args[i] = cpu_to_le32(desc->args[i +
						      FIRST_EXT_ARG_IDX]);
		} else {
			__le64 *args = args_virt;

			for (i = 0; i < N_EXT_QCOM_SCM_ARGS; i++)
				args[i] = cpu_to_le64(desc->args[i +
						      FIRST_EXT_ARG_IDX]);
		}

		args_phys = dma_map_single(dev, args_virt, alloc_len,
					   DMA_TO_DEVICE);

		if (dma_mapping_error(dev, args_phys)) {
			kfree(args_virt);
			return -ENOMEM;
		}

		x5 = args_phys;
	}

	do {
		mutex_lock(&qcom_scm_lock);

		cmd = ARM_SMCCC_CALL_VAL(ARM_SMCCC_STD_CALL,
					 qcom_smccc_convention,
					 ARM_SMCCC_OWNER_SIP, fn_id);

		do {
			arm_smccc_smc(cmd, desc->arginfo, desc->args[0],
				      desc->args[1], desc->args[2], x5, 0, 0,
				      res, NULL);
		} while (res->a0 == QCOM_SCM_INTERRUPTED);

		mutex_unlock(&qcom_scm_lock);

		if (res->a0 == QCOM_SCM_V2_EBUSY) {
			if (retry_count++ > QCOM_SCM_EBUSY_MAX_RETRY)
				break;
			msleep(QCOM_SCM_EBUSY_WAIT_MS);
		}
	}  while ((signed long)res->a0 == QCOM_SCM_V2_EBUSY);

	if (args_virt) {
		dma_unmap_single(dev, args_phys, alloc_len, DMA_TO_DEVICE);
		kfree(args_virt);
	}

	if (res->a0) {
		dev_err(dev, "%s: error %lx (a1:%pa, a2:%pa, a3:%pa)\n", __func__,
			res->a0, &res->a1, &res->a2, &res->a3);
		return qcom_scm_remap_error(res->a0);
	}

	return 0;
}
#endif
/**
 * qcom_scm_set_cold_boot_addr() - Set the cold boot address for cpus
 * @entry: Entry point function for the cpus
 * @cpus: The cpumask of cpus that will use the entry point
 *
 * Set the cold boot address of the cpus. Any cpu outside the supported
 * range would be removed from the cpu present mask.
 */
int __qcom_scm_set_cold_boot_addr(void *entry, const cpumask_t *cpus)
{
	return -ENOTSUPP;
}

/**
 * qcom_scm_set_warm_boot_addr() - Set the warm boot address for cpus
 * @dev: Device pointer
 * @entry: Entry point function for the cpus
 * @cpus: The cpumask of cpus that will use the entry point
 *
 * Set the Linux entry point for the SCM to transfer control to when coming
 * out of a power down. CPU power down may be executed on cpuidle or hotplug.
 */
int __qcom_scm_set_warm_boot_addr(struct device *dev, void *entry,
				  const cpumask_t *cpus)
{
	return -ENOTSUPP;
}

/**
 * qcom_scm_cpu_power_down() - Power down the cpu
 * @flags - Flags to flush cache
 *
 * This is an end point to power down cpu. If there was a pending interrupt,
 * the control would return from this function, otherwise, the cpu jumps to the
 * warm boot entry point set for this cpu upon reset.
 */
void __qcom_scm_cpu_power_down(u32 flags)
{
}

int __qcom_scm_is_call_available(struct device *dev, u32 svc_id, u32 cmd_id)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	desc.arginfo = QCOM_SCM_ARGS(1);
	desc.args[0] = QCOM_SCM_FNID(svc_id, cmd_id) |
			(ARM_SMCCC_OWNER_SIP << ARM_SMCCC_OWNER_SHIFT);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_INFO, QCOM_IS_CALL_AVAIL_CMD,
			    &desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_hdcp_req(struct device *dev, struct qcom_scm_hdcp_req *req,
			u32 req_cnt, u32 *resp)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	if (req_cnt > QCOM_SCM_HDCP_MAX_REQ_CNT)
		return -ERANGE;

	desc.args[0] = req[0].addr;
	desc.args[1] = req[0].val;
	desc.args[2] = req[1].addr;
	desc.args[3] = req[1].val;
	desc.args[4] = req[2].addr;
	desc.args[5] = req[2].val;
	desc.args[6] = req[3].addr;
	desc.args[7] = req[3].val;
	desc.args[8] = req[4].addr;
	desc.args[9] = req[4].val;
	desc.arginfo = QCOM_SCM_ARGS(10);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_HDCP, QCOM_SCM_CMD_HDCP, &desc,
			    &res);
	*resp = res.a1;

	return ret;
}

void __qcom_scm_init(void)
{
	u64 cmd;
	struct arm_smccc_res res;
	u32 function = QCOM_SCM_FNID(QCOM_SCM_SVC_INFO, QCOM_IS_CALL_AVAIL_CMD);

	/* First try a SMC64 call */
	cmd = ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				 ARM_SMCCC_OWNER_SIP, function);

	arm_smccc_smc(cmd, QCOM_SCM_ARGS(1), cmd & (~BIT(ARM_SMCCC_TYPE_SHIFT)),
		      0, 0, 0, 0, 0, &res, NULL);

	if (!res.a0 && res.a1)
		qcom_smccc_convention = ARM_SMCCC_SMC_64;
	else
		qcom_smccc_convention = ARM_SMCCC_SMC_32;
}

bool __qcom_scm_pas_supported(struct device *dev, u32 peripheral)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	desc.args[0] = peripheral;
	desc.arginfo = QCOM_SCM_ARGS(1);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_PIL,
				QCOM_SCM_PAS_IS_SUPPORTED_CMD,
				&desc, &res);

	return ret ? false : !!res.a1;
}

int __qcom_scm_pas_init_image(struct device *dev, u32 peripheral,
			      dma_addr_t metadata_phys)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	desc.args[0] = peripheral;
	desc.args[1] = metadata_phys;
	desc.arginfo = QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_RW);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_INIT_IMAGE_CMD,
				&desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_pas_mem_setup(struct device *dev, u32 peripheral,
			      phys_addr_t addr, phys_addr_t size)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	desc.args[0] = peripheral;
	desc.args[1] = addr;
	desc.args[2] = size;
	desc.arginfo = QCOM_SCM_ARGS(3);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_MEM_SETUP_CMD,
				&desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_pas_auth_and_reset(struct device *dev, u32 peripheral)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	desc.args[0] = peripheral;
	desc.arginfo = QCOM_SCM_ARGS(1);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_PIL,
				QCOM_SCM_PAS_AUTH_AND_RESET_CMD,
				&desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_pas_shutdown(struct device *dev, u32 peripheral)
{
	int ret;
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;

	desc.args[0] = peripheral;
	desc.arginfo = QCOM_SCM_ARGS(1);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN_CMD,
			&desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_pas_mss_reset(struct device *dev, bool reset)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = reset;
	desc.args[1] = 0;
	desc.arginfo = QCOM_SCM_ARGS(2);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_MSS_RESET, &desc,
			    &res);

	return ret ? : res.a1;
}

int __qcom_scm_video_set_state(struct device *dev, u32 state, u32 spare)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = state;
	desc.args[1] = spare;
	desc.arginfo = QCOM_SCM_ARGS(2);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_BOOT, QCOM_SCM_VIDEO_SET_STATE,
			    &desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_iommu_secure_ptbl_size(struct device *dev, u32 spare,
				      size_t *size)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = spare;
	desc.arginfo = QCOM_SCM_ARGS(1);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_MP,
			    QCOM_SCM_IOMMU_SECURE_PTBL_SIZE, &desc, &res);

	if (size)
		*size = res.a1;

	return ret ? : res.a2;
}

int __qcom_scm_iommu_secure_ptbl_init(struct device *dev, u64 addr, u32 size,
				      u32 spare)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = addr;
	desc.args[1] = size;
	desc.args[2] = spare;
	desc.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_RW, QCOM_SCM_VAL,
				     QCOM_SCM_VAL);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_MP,
			    QCOM_SCM_IOMMU_SECURE_PTBL_INIT, &desc, &res);

	/* the pg table has been initialized already, ignore the error */
	if (ret == -EPERM)
		ret = 0;

	return ret;
}

int __qcom_scm_iommu_dump_fault_regs(struct device *dev, u32 id, u32 context,
				     u64 addr, u32 len)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = id;
	desc.args[1] = context;
	desc.args[2] = addr;
	desc.args[3] = len;
	desc.arginfo = QCOM_SCM_ARGS(4, QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_RW,
				     QCOM_SCM_VAL);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_UTIL,
			    QCOM_SCM_IOMMU_DUMP_SMMU_FAULT_REGS, &desc, &res);
	return ret ? : res.a1;
}

int __qcom_scm_restore_sec_cfg(struct device *dev, u32 device_id, u32 spare)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = device_id;
	desc.args[1] = spare;
	desc.arginfo = QCOM_SCM_ARGS(2);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_MP, QCOM_SCM_RESTORE_SEC_CFG,
			    &desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_iommu_secure_map(struct device *dev, u64 list, u32 list_size,
				u32 size, u32 id, u32 ctx_id, u64 va,
				u32 info_size, u32 flags)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = list;
	desc.args[1] = list_size;
	desc.args[2] = size;
	desc.args[3] = id;
	desc.args[4] = ctx_id;
	desc.args[5] = va;
	desc.args[6] = info_size;
	desc.args[7] = flags;
	desc.arginfo = QCOM_SCM_ARGS(8, QCOM_SCM_RW, QCOM_SCM_VAL, QCOM_SCM_VAL,
				     QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_VAL,
				     QCOM_SCM_VAL, QCOM_SCM_VAL);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_MP,
			    QCOM_SCM_IOMMU_SECURE_MAP2_FLAT, &desc, &res);

	return ret ? : res.a1;
}

int __qcom_scm_iommu_secure_unmap(struct device *dev, u32 id, u32 ctx_id,
				  u64 va, u32 size, u32 flags)
{
	struct qcom_scm_desc desc = {0};
	struct arm_smccc_res res;
	int ret;

	desc.args[0] = id;
	desc.args[1] = ctx_id;
	desc.args[2] = va;
	desc.args[3] = size;
	desc.args[4] = flags;
	desc.arginfo = QCOM_SCM_ARGS(5);

	ret = qcom_scm_call(dev, QCOM_SCM_SVC_MP,
			    QCOM_SCM_IOMMU_SECURE_UNMAP2_FLAT, &desc, &res);
	return ret ? : res.a1;
}
