/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Universal Flash Storage File-based Optimization
 *
 * Copyright (C) 2022 Xiaomi Mobile Software Co., Ltd
 *
 * Authors:
 *		lijiaming <lijiaming3@xiaomi.com>
 */

#ifndef _UFSFBO_H_
#define _UFSFBO_H_

#ifdef CONFIG_SCSI_UFS_FBO
int ufsfbo_probe(struct ufs_hba *hba, const u8 *desc_buf);
void ufsfbo_remove(struct ufs_hba *hba);
extern const struct attribute_group ufs_sysfs_fbo_param_group;
#else
static inline int ufsfbo_probe(struct ufs_hba *hba, const u8 *desc_buf) {}
static inline void ufsfbo_remove(struct ufs_hba *hba) {}
#endif

#endif /* _UFSFBO_H_ */
