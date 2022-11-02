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

#define FBO_RW_BUF_HDR_SIZE 4
#define FBO_RW_ENTRY_SIZE 8
#define FBO_LBA_RANGE_LENGTH 4096

enum UFSFBO_PROG_STATE {
	FBO_PROG_IDLE   = 0x0,
	FBO_PROG_ON_GOING   = 0x1,
	FBO_PROG_ANALYSIS_COMPLETE  = 0x2,
	FBO_PROG_OPTIMIZATION_COMPLETE  = 0x3,
	FBO_PROG_INTERNAL_ERR   = 0xff,
};


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

static void ufsfbo_fill_rw_buffer(unsigned char *cdb, int size, u8 opcode)
{
	cdb[0] = opcode;
	cdb[1] = 0x2;
	cdb[2] = opcode == WRITE_BUFFER ? 0x1 : 0x2;
	put_unaligned_be24(size, &cdb[6]);
}

static ssize_t fbo_support_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);
	u8 val = 0;

	if (hba->fbo_ctrl)
		val = 1;

	return sysfs_emit(buf, "%d\n", val);
}
static DEVICE_ATTR_RO(fbo_support);

static int ufsfbo_get_fbo_prog_state(struct ufs_hba *hba, int *prog_state)
{
	int ret = 0, attr = -1;

	down(&hba->host_sem);
	if (!ufshcd_is_user_access_allowed(hba)) {
		ret = -EBUSY;
		goto out;
	}
	ufshcd_rpm_get_sync(hba);
	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_READ_ATTR,
			QUERY_ATTR_IDN_FBO_PROG_STATE, 0, 0, &attr);
	ufshcd_rpm_put_sync(hba);
	if (ret) {
		pr_err("Query attr fbo prog state failed.");
		goto out;
	}

	switch (attr) {
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
	case 0xff:
		*prog_state = attr;
		break;
	default:
		pr_info("Unknown fbo prog state attr(%d)", attr);
		ret = -EINVAL;
		break;
	}

out:
	up(&hba->host_sem);
	return ret;
}

static ssize_t fbo_prog_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	int fbo_prog_state;
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);

	if (ufsfbo_get_fbo_prog_state(hba, &fbo_prog_state)) {
		pr_err("Get fbo prog state failed.");
		return -EINVAL;
	}

	return sysfs_emit(buf, "%d\n", fbo_prog_state);
}
static DEVICE_ATTR_RO(fbo_prog_state);

static ssize_t fbo_operation_ctrl_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int ret = 0;
	u32 val;
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;

	down(&hba->host_sem);
	if (!ufshcd_is_user_access_allowed(hba)) {
		ret = -EBUSY;
		goto out;
	}

	ufshcd_rpm_get_sync(hba);
	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
			QUERY_ATTR_IDN_FBO_CONTROL, 0, 0, &val);
	ufshcd_rpm_put_sync(hba);

out:
	up(&hba->host_sem);
	return ret ? ret : count;
}

static DEVICE_ATTR_WO(fbo_operation_ctrl);

static ssize_t fbo_exe_threshold_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);
	struct ufsfbo_ctrl *fbo_ctrl = hba->fbo_ctrl;

	return sysfs_emit(buf, "%d\n", fbo_ctrl->fbo_dev_info.fbo_exec_threshold);
}

static int ufsfbo_set_exe_level(struct ufs_hba *hba, u32 val)
{
	int ret = 0, fbo_prog_state = 0;

	ret = ufsfbo_get_fbo_prog_state(hba, &fbo_prog_state);
	if (ret) {
		pr_err("Get fbo prog state failed.");
		return -EINVAL;
	}

	if (fbo_prog_state == FBO_PROG_IDLE || fbo_prog_state == FBO_PROG_ANALYSIS_COMPLETE ||
		fbo_prog_state == FBO_PROG_OPTIMIZATION_COMPLETE) {
		down(&hba->host_sem);
		if (!ufshcd_is_user_access_allowed(hba)) {
			ret = -EBUSY;
			goto out;
		}
		ufshcd_rpm_get_sync(hba);
		ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
				QUERY_ATTR_IDN_FBO_LEVEL_EXE, 0, 0, &val);
		ufshcd_rpm_put_sync(hba);
	} else {
		pr_err("Illegal fbo prog state");
		return -EINVAL;
	}

out:
	up(&hba->host_sem);
	return ret;
}

static ssize_t fbo_exe_threshold_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	u32 val;
	int ret = 0;
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);
	struct ufsfbo_ctrl *fbo_ctrl = hba->fbo_ctrl;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;

	if (val < 0 || val > 10)
		return -EINVAL;

	ret = ufsfbo_set_exe_level(hba, val);
	if (ret) {
		pr_err("Set exec threshold failed.");
		return -EINVAL;
	}

	fbo_ctrl->fbo_dev_info.fbo_exec_threshold = val;

	return ret ? ret : count;
}

static DEVICE_ATTR_RW(fbo_exe_threshold);

static int ufsfbo_issue_read_frag_level(struct scsi_device *sdev, char *buf, int para_len)
{
	int ret = 0;
	unsigned char cdb[10] = {};
	struct scsi_sense_hdr sshdr = {};

	ufsfbo_fill_rw_buffer(cdb, para_len, READ_BUFFER);

	ret = scsi_execute_req(sdev, cdb, DMA_FROM_DEVICE, buf, para_len,
			&sshdr, msecs_to_jiffies(15000), 0, NULL);
	if (ret)
		pr_err("Read Buffer failed,sense key:0x%x;asc:0x%x;ascq:0x%x",
			(int)sshdr.sense_key, (int)sshdr.asc, (int)sshdr.ascq);

	return ret;
}

static ssize_t fbo_lba_frag_state_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	int i, ret, count = 0;
	int para_len = 0;
	int vaild_body_size = 0;
	char *fbo_read_buffer;
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);
	struct ufsfbo_ctrl *fbo_ctrl = hba->fbo_ctrl;

	fbo_read_buffer = kzalloc(FBO_LBA_RANGE_LENGTH, GFP_KERNEL);
	if (!fbo_read_buffer)
		return -ENOMEM;

	para_len = FBO_RW_BUF_HDR_SIZE + FBO_RW_ENTRY_SIZE +
		fbo_ctrl->fbo_lba_cnt * FBO_RW_ENTRY_SIZE;

	ret = ufsfbo_issue_read_frag_level(sdev, fbo_read_buffer, para_len);
	if (ret) {
		pr_err("Get lba range level failed");
		goto out;
	}

	/* we allocated 4k, but reading only the relevant ReadBuffer size */
	vaild_body_size = FBO_RW_ENTRY_SIZE + (fbo_ctrl->fbo_lba_cnt * FBO_RW_ENTRY_SIZE);
	for (i = 0; i < vaild_body_size; i++) {
		count += snprintf(buf + count, PAGE_SIZE - count,
				"%02x  ", fbo_read_buffer[i + FBO_RW_BUF_HDR_SIZE]);
		if (!((i + 1) % 8))
			count += snprintf(buf + count, PAGE_SIZE - count, "\n");
	}
out:
	kfree(fbo_read_buffer);
	return ret ? ret : count;
}

static DEVICE_ATTR_RO(fbo_lba_frag_state);

static int ufsfbo_check_lba_range_format(struct ufs_hba *hba, char *buf)
{
	char *p;
	int lba_pairs = 0;
	struct ufsfbo_ctrl *fbo_ctrl = hba->fbo_ctrl;

	p = strstr(buf, ",");
	if (!p || buf[strlen(buf) - 1] == ',') {
		pr_err("Invalid lba range format, input lba range separated by ','");
		return -EINVAL;
	}

	while (p) {
		lba_pairs++;
		p += 1;
		p = strstr(p, ",");
	}
	/*
	 * The input buffer is a comma delimited pairs of LBAs: open,close,
	 * and so on.  So there should be an even number of LBAs, and odd
	 * number of commas.
	 */
	if (lba_pairs % 2)
		lba_pairs++;
	else
		return -EINVAL;

	if (lba_pairs / 2 > fbo_ctrl->fbo_dev_info.fbo_max_lrc)
		return -EINVAL;

	fbo_ctrl->fbo_lba_cnt = lba_pairs / 2;
	return 0;
}

static int ufsfbo_parse_lba_list(struct ufs_hba *hba, char *buf, char *lba_buf)
{
	char *lba_ptr;
	struct ufsfbo_ctrl *fbo_ctrl = hba->fbo_ctrl;
	struct ufsfbo_dev_info *fbo_dev_info = &fbo_ctrl->fbo_dev_info;
	u64 lba_range_tmp, start_lba, lba_len;
	int len_index = 1, lba_info_offset = FBO_RW_BUF_HDR_SIZE + FBO_RW_ENTRY_SIZE;

	lba_buf[5] = fbo_ctrl->fbo_lba_cnt;

	while ((lba_ptr = strsep(&buf, ",")) != NULL) {
		if (kstrtou64(lba_ptr, 16, &lba_range_tmp))
			return -EINVAL;

		if (len_index % 2) {
			start_lba = lba_range_tmp;
			put_unaligned_be32(start_lba, lba_buf + lba_info_offset);
		} else {
			if (lba_range_tmp < start_lba)
				return -EINVAL;

			lba_len = lba_range_tmp - start_lba + 1;
			if (lba_len < fbo_dev_info->fbo_min_lrs ||
				lba_len > fbo_dev_info->fbo_max_lrs)
				return -EINVAL;

			put_unaligned_be24(lba_len, lba_buf + lba_info_offset + 4);
			lba_info_offset += FBO_RW_ENTRY_SIZE;
		}
		len_index++;
	}

	return 0;
}

static int ufsfbo_issue_lba_list_write(struct scsi_device *sdev, char *buf)
{
	int ret = 0;
	struct ufs_hba *hba = shost_priv(sdev->host);
	struct ufsfbo_ctrl *fbo_ctrl = hba->fbo_ctrl;
	int fbo_lba_cnt = fbo_ctrl->fbo_lba_cnt;
	struct scsi_sense_hdr sshdr = {};
	char *buf_lba;
	unsigned char cdb[10] = {};
	int para_len = FBO_RW_BUF_HDR_SIZE + FBO_RW_ENTRY_SIZE + fbo_lba_cnt * FBO_RW_ENTRY_SIZE;

	buf_lba = kzalloc(FBO_LBA_RANGE_LENGTH, GFP_KERNEL);
	if (!buf_lba) {
		ret = -ENOMEM;
		return ret;
	}

	ret = ufsfbo_parse_lba_list(hba, buf, buf_lba);
	if (ret) {
		pr_err("Init buf_lba fail");
		goto out;
	}

	ufsfbo_fill_rw_buffer(cdb, para_len, WRITE_BUFFER);

	ret = scsi_execute_req(sdev, cdb, DMA_TO_DEVICE, buf_lba, para_len,
			&sshdr, msecs_to_jiffies(15000), 0, NULL);
	if (ret)
		pr_err("Write Buffer failed,sense key:0x%x;asc:0x%x;ascq:0x%x",
			(int)sshdr.sense_key, (int)sshdr.asc, (int)sshdr.ascq);

out:
	kfree(buf_lba);
	return ret;
}

static ssize_t fbo_send_lba_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int ret = 0, fbo_prog_state = 0;
	char *buf_ptr;
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ufs_hba *hba = shost_priv(sdev->host);

	if (!buf)
		return -EINVAL;

	buf_ptr = kstrdup(buf, GFP_KERNEL);
	if (unlikely(!buf_ptr))
		return -ENOMEM;

	if (ufsfbo_check_lba_range_format(hba, buf_ptr))
		goto out;

	if (ufsfbo_get_fbo_prog_state(hba, &fbo_prog_state))
		goto out;

	if (fbo_prog_state == FBO_PROG_IDLE) {
		ret = ufsfbo_issue_lba_list_write(sdev, buf_ptr);
	} else {
		ret = -EINVAL;
		pr_err("Invalid fbo state");
	}

out:
	kfree(buf_ptr);
	return ret ? ret : count;
}

static DEVICE_ATTR_WO(fbo_send_lba);

static struct attribute *fbo_dev_ctrl_attrs[] = {
	&dev_attr_fbo_support.attr,
	&dev_attr_fbo_prog_state.attr,
	&dev_attr_fbo_operation_ctrl.attr,
	&dev_attr_fbo_exe_threshold.attr,
	&dev_attr_fbo_send_lba.attr,
	&dev_attr_fbo_lba_frag_state.attr,
	NULL,
};

const struct attribute_group ufs_sysfs_fbo_param_group = {
	.name = "fbo_dev_ctrl",
	.attrs = fbo_dev_ctrl_attrs,
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
