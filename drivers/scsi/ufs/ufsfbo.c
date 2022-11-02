// SPDX-License-Identifier: GPL-2.0
/*
 * Universal Flash Storage File-based Optimization
 *
 * Copyright (C) 2022 Xiaomi Mobile Software Co., Ltd
 *
 * Authors:
 *		lijiaming <lijiaming3@xiaomi.com>
 */

#include "ufshcd-priv.h"
#include "ufsfbo.h"
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <asm/unaligned.h>

/**
 * struct ufsfbo_dev_info - FBO device related info
 * @fbo_version: UFS file-based optimization Version
 * @fbo_rec_lrs: Recommended LBA Range Size In Bytes
 * @fbo_max_lrs: The Max LBA Range Size To Be Used By The Host
 * @fbo_min_lrs: The Min LBA Range Size To Be Used By The Host
 * @fbo_max_lrc: The Max Number Of LBA Ranges Supported By Read/Write Buffer Cmd
 * @fbo_lra: Alignment Requirement. 0 Means No Align Requirement
 * @fbo_exec_threshold: the execute level of UFS file-based optimization
 */
struct ufsfbo_dev_info {
	u16 fbo_version;
	u32 fbo_rec_lrs;
	u32 fbo_max_lrs;
	u32 fbo_min_lrs;
	int fbo_max_lrc;
	int fbo_lra;
	u8  fbo_exec_threshold;
};
/**
 * struct ufsfbo_ctrl - FBO ctrl structure
 * @hba: Per adapter private structure
 * @fbo_dev_info: FBO device related info
 * @fbo_lba_cnt: Number of LBA required to do FBO
 */
struct ufsfbo_ctrl {
	struct ufs_hba *hba;
	struct ufsfbo_dev_info fbo_dev_info;
	int fbo_lba_cnt;
};

static int ufsfbo_get_dev_info(struct ufs_hba *hba, struct ufsfbo_ctrl *fbo_ctrl)
{
	int ret;
	u32 val;
	u8 *desc_buf;
	int buf_len;
	struct ufsfbo_dev_info *fbo_info = &fbo_ctrl->fbo_dev_info;

	buf_len = hba->desc_size[QUERY_DESC_IDN_FBO];
	desc_buf = kmalloc(buf_len, GFP_KERNEL);
	if (!desc_buf)
		return -ENOMEM;

	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_READ_DESC,
					QUERY_DESC_IDN_FBO, 0, 0, desc_buf, &buf_len);
	if (ret) {
		dev_err(hba->dev, "%s: Failed reading FBO Desc. ret = %d\n",
				__func__, ret);
		goto out;
	}

	fbo_info->fbo_version = get_unaligned_be16(desc_buf + FBO_DESC_PARAM_VERSION);
	fbo_info->fbo_rec_lrs = get_unaligned_be32(desc_buf + FBO_DESC_PARAM_REC_LBA_RANGE_SIZE);
	fbo_info->fbo_max_lrs = get_unaligned_be32(desc_buf + FBO_DESC_PARAM_MAX_LBA_RANGE_SIZE);
	fbo_info->fbo_min_lrs = get_unaligned_be32(desc_buf + FBO_DESC_PARAM_MIN_LBA_RANGE_SIZE);
	fbo_info->fbo_max_lrc = desc_buf[FBO_DESC_PARAM_MAX_LBA_RANGE_CONUT];
	fbo_info->fbo_lra = get_unaligned_be16(desc_buf + FBO_DESC_PARAM_MAX_LBA_RANGE_ALIGNMENT);

	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_READ_ATTR,
				QUERY_ATTR_IDN_FBO_LEVEL_EXE, 0, 0, &val);
	if (ret) {
		dev_err(hba->dev, "%s: Failed reading FBO Attr. ret = %d\n",
				__func__, ret);
		goto out;
	}

	fbo_info->fbo_exec_threshold = val;

out:
	kfree(desc_buf);
	return ret;
}

int ufsfbo_probe(struct ufs_hba *hba, const u8 *desc_buf)
{
	struct ufsfbo_ctrl *fbo_ctrl;
	u32 ext_ufs_feature;

	ext_ufs_feature = get_unaligned_be32(desc_buf + DEVICE_DESC_PARAM_EXT_UFS_FEATURE_SUP);

	if (!(ext_ufs_feature & UFS_DEV_FBO_SUP))
		return -EOPNOTSUPP;

	fbo_ctrl = kzalloc(sizeof(struct ufsfbo_ctrl), GFP_KERNEL);
	if (!fbo_ctrl)
		return -ENOMEM;

	if (ufsfbo_get_dev_info(hba, fbo_ctrl))
		return -EOPNOTSUPP;

	hba->fbo_ctrl = fbo_ctrl;
	fbo_ctrl->hba = hba;

	return 0;
}

void ufsfbo_remove(struct ufs_hba *hba)
{
	if (!hba->fbo_ctrl)
		return;

	kfree(hba->fbo_ctrl);
}
