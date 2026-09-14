extern struct workqueue_struct *nvt_fwu_wq;

#define BLD_BANK_ADDR 0x1FB500
#define RD32(o) ((uint32_t)buf[1 + (o)] | ((uint32_t)buf[1 + (o) + 1] << 8) | \
		 ((uint32_t)buf[1 + (o) + 2] << 16) | ((uint32_t)buf[1 + (o) + 3] << 24))
#define RD24(o) ((uint32_t)buf[1 + (o)] | ((uint32_t)buf[1 + (o) + 1] << 8) | \
		 ((uint32_t)buf[1 + (o) + 2] << 16))
/*
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 *
 * $Revision: 32206 $
 * $Date: 2018-08-10 19:23:04 +0800 (週五, 10 八月 2018) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <asm/uaccess.h>

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/moduleparam.h>

#include "nt36xxx.h"

#if BOOT_UPDATE_FIRMWARE

/* 见 nvt_write_sram()：默认关闭的逐笔回读校验 */
static bool nvt_fwu_verify;
module_param_named(fwu_verify, nvt_fwu_verify, bool, 0644);
MODULE_PARM_DESC(fwu_verify, "Read back every SRAM write while flashing (debug only)");

/* 2026-09-14 A/B knob: skip the cascade tx-auto-copy handshake */
static bool nvt_no_autocopy;
module_param_named(no_autocopy, nvt_no_autocopy, bool, 0644);
MODULE_PARM_DESC(no_autocopy, "skip tx_auto_copy_mode/wait_auto_copy in hw_crc download");

#define SIZE_4KB 4096
#define FLASH_SECTOR_SIZE SIZE_4KB
#define FW_BIN_VER_OFFSET (fw_need_write_size - SIZE_4KB)
#define FW_BIN_VER_BAR_OFFSET (FW_BIN_VER_OFFSET + 1)
#define NVT_FLASH_END_FLAG_LEN 3
#define NVT_FLASH_END_FLAG_ADDR (fw_need_write_size - NVT_FLASH_END_FLAG_LEN)

#define NVT_DUMP_PARTITION			(0)
#define NVT_DUMP_PARTITION_LEN		(1024)
#define NVT_DUMP_PARTITION_PATH		"/data/local/tmp"

static ktime_t start, end;
const struct firmware *fw_entry = NULL;
static size_t fw_need_write_size = 0;
static uint8_t *fwbuf = NULL;

struct nvt_ts_bin_map {
	char name[12];
	uint32_t BIN_addr;
	uint32_t SRAM_addr;
	uint32_t size;
	uint32_t crc;
};

static struct nvt_ts_bin_map *bin_map;

static int32_t nvt_get_fw_need_write_size(const struct firmware *fw_entry)
{
	int32_t i = 0;
	int32_t total_sectors_to_check = 0;

	total_sectors_to_check = fw_entry->size / FLASH_SECTOR_SIZE;
	/* printk("total_sectors_to_check = %d\n", total_sectors_to_check); */

	for (i = total_sectors_to_check; i > 0; i--) {
		/* printk("current end flag address checked = 0x%X\n", i * FLASH_SECTOR_SIZE - NVT_FLASH_END_FLAG_LEN); */
		/* check if there is end flag "NVT" at the end of this sector */
		if (strncmp(&fw_entry->data[i * FLASH_SECTOR_SIZE - NVT_FLASH_END_FLAG_LEN], "NVT", NVT_FLASH_END_FLAG_LEN) == 0) {
			fw_need_write_size = i * FLASH_SECTOR_SIZE;
			NVT_LOG("fw_need_write_size = %zu(0x%zx), NVT end flag\n", fw_need_write_size, fw_need_write_size);
			return 0;
		}

		/* check if there is end flag "MOD" at the end of this sector */
		if (strncmp(&fw_entry->data[i * FLASH_SECTOR_SIZE - NVT_FLASH_END_FLAG_LEN], "MOD", NVT_FLASH_END_FLAG_LEN) == 0) {
			fw_need_write_size = i * FLASH_SECTOR_SIZE;
			NVT_LOG("fw_need_write_size = %zu(0x%zx), MOD end flag\n", fw_need_write_size, fw_need_write_size);
			return 0;
		}
	}

	NVT_ERR("end flag \"NVT\" \"MOD\" not found!\n");
	return -1;
}

/*******************************************************
Description:
	Novatek touchscreen init variable and allocate buffer
for download firmware function.

return:
	n.a.
*******************************************************/
static int32_t nvt_download_init(void)
{
	/* allocate buffer for transfer firmware */
	/* NVT_LOG("NVT_TRANSFER_LEN = 0x%06X\n", NVT_TRANSFER_LEN); */

	if (fwbuf == NULL) {
		fwbuf = (uint8_t *)kzalloc((NVT_TRANSFER_LEN + 1 + DUMMY_BYTES), GFP_KERNEL);
		if(fwbuf == NULL) {
			NVT_ERR("kzalloc for fwbuf failed!\n");
			return -ENOMEM;
		}
	}

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen checksum function. Calculate bin
file checksum for comparison.

return:
	n.a.
*******************************************************/
static uint32_t CheckSum(const u8 *data, size_t len)
{
	uint32_t i = 0;
	uint32_t checksum = 0;

	for (i = 0 ; i < len+1 ; i++)
		checksum += data[i];

	checksum += len;
	checksum = ~checksum +1;

	return checksum;
}

static uint32_t byte_to_word(const uint8_t *data)
{
	return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}

/*******************************************************
Description:
	Novatek touchscreen parsing bin header function.

return:
	n.a.
*******************************************************/
static uint32_t partition = 0;
static uint8_t ilm_dlm_num = 2;
static int32_t nvt_bin_header_parser(const u8 *fwdata, size_t fwsize)
{
	uint32_t list = 0;
	uint32_t pos = 0x00;
	uint32_t end = 0x00;
	uint8_t info_sec_num = 0;
	uint8_t ovly_sec_num = 0;
	uint8_t ovly_info = 0;

	/* Find the header size */
	end = fwdata[0] + (fwdata[1] << 8) + (fwdata[2] << 16) + (fwdata[3] << 24);
	if (fwdata[0x20] & 0x02) {
		NVT_LOG("Cascade 2nd header detected (0x%02X), adjusting header size from 0x%X to 0x%X\n", fwdata[0x20], end, end / 2);
		end = end / 2;
	}
	pos = 0x30;	/* info section start at 0x30 offset */
	while (pos < end) {
		info_sec_num ++;
		pos += 0x10; /* each header info is 16 bytes */
	}

	/*
	 * Find the DLM OVLY section
	 * [0:3] Overlay Section Number
	 * [4]   Overlay Info
	 */
	ovly_info = (fwdata[0x28] & 0x10) >> 4;
	ovly_sec_num = (ovly_info) ? (fwdata[0x28] & 0x0F) : 0;

	/*
	 * calculate all partition number
	 * ilm_dlm_num (ILM & DLM) + ovly_sec_num + info_sec_num
	 */
	partition = ilm_dlm_num + ovly_sec_num + info_sec_num;
	NVT_LOG("ovly_info = %d, ilm_dlm_num = %d, ovly_sec_num = %d, info_sec_num = %d, partition = %d\n",
			ovly_info, ilm_dlm_num, ovly_sec_num, info_sec_num, partition);

	/* allocated memory for header info */
	bin_map = (struct nvt_ts_bin_map *)kzalloc((partition+1) * sizeof(struct nvt_ts_bin_map), GFP_KERNEL);
	if(bin_map == NULL) {
		NVT_ERR("kzalloc for bin_map failed!\n");
		return -ENOMEM;
	}

	for (list = 0; list < partition; list++) {
		/*
		 * [1] parsing ILM & DLM header info
		 * BIN_addr : SRAM_addr : size (12-bytes)
		 * crc located at 0x18 & 0x1C
		 */
		if (list < ilm_dlm_num) {
			bin_map[list].BIN_addr = byte_to_word(&fwdata[0 + list*12]);
			bin_map[list].SRAM_addr = byte_to_word(&fwdata[4 + list*12]);
			bin_map[list].size = byte_to_word(&fwdata[8 + list*12]);
			if (ts->hw_crc)
				bin_map[list].crc = byte_to_word(&fwdata[0x18 + list*4]);
			else { //ts->hw_crc
				if ((bin_map[list].BIN_addr + bin_map[list].size) < fwsize)
					bin_map[list].crc = CheckSum(&fwdata[bin_map[list].BIN_addr], bin_map[list].size);
				else {
					NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
							bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
					return -EINVAL;
				}
			}
			if (list == 0)
				sprintf(bin_map[list].name, "ILM");
			else if (list == 1)
				sprintf(bin_map[list].name, "DLM");
		}

		/*
		 * [2] parsing others header info
		 * SRAM_addr : size : BIN_addr : crc (16-bytes)
		 */
		if ((list >= ilm_dlm_num) && (list < (ilm_dlm_num + info_sec_num))) {
			/* others partition located at 0x30 offset */
			pos = 0x30 + (0x10 * (list - ilm_dlm_num));

			bin_map[list].SRAM_addr = byte_to_word(&fwdata[pos]);
			bin_map[list].size = byte_to_word(&fwdata[pos+4]);
			bin_map[list].BIN_addr = byte_to_word(&fwdata[pos+8]);
			if (ts->hw_crc)
				bin_map[list].crc = byte_to_word(&fwdata[pos+12]);
			else {
				if ((bin_map[list].BIN_addr + bin_map[list].size) < fwsize)
					bin_map[list].crc = CheckSum(&fwdata[bin_map[list].BIN_addr], bin_map[list].size);
				else {
					NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
							bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
					return -EINVAL;
				}
			}
			/* detect header end to protect parser function */
			if ((bin_map[list].BIN_addr == 0) && (bin_map[list].size != 0)) {
				sprintf(bin_map[list].name, "Header");
			} else {
				sprintf(bin_map[list].name, "Info-%d", (list - ilm_dlm_num));
			}
		}

		/*
		 * [3] parsing overlay section header info
		 * SRAM_addr : size : BIN_addr : crc (16-bytes)
		 */
		if (list >= (ilm_dlm_num + info_sec_num)) {
			/* overlay info located at DLM (list = 1) start addr */
			pos = bin_map[1].BIN_addr + (0x10 * (list- ilm_dlm_num - info_sec_num));

			bin_map[list].SRAM_addr = byte_to_word(&fwdata[pos]);
			bin_map[list].size = byte_to_word(&fwdata[pos+4]);
			bin_map[list].BIN_addr = byte_to_word(&fwdata[pos+8]);
			if (ts->hw_crc)
				bin_map[list].crc = byte_to_word(&fwdata[pos+12]);
			else {
				if ((bin_map[list].BIN_addr + bin_map[list].size) < fwsize)
					bin_map[list].crc = CheckSum(&fwdata[bin_map[list].BIN_addr], bin_map[list].size);
				else {
					NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
							bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
					return -EINVAL;
				}
			}
			sprintf(bin_map[list].name, "Overlay-%d", (list- ilm_dlm_num - info_sec_num));
		}

		/* BIN size error detect */
		if ((bin_map[list].BIN_addr + bin_map[list].size) > fwsize) {
			NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
					bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
			return -EINVAL;
		}

	/* NVT_LOG("[%d][%s] SRAM (0x%08X), SIZE (0x%08X), BIN (0x%08X), CRC (0x%08X)\n",
			list, bin_map[list].name,
			bin_map[list].SRAM_addr, bin_map[list].size,  bin_map[list].BIN_addr, bin_map[list].crc); */
	}

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen release update firmware function.

return:
	n.a.
*******************************************************/
static void update_firmware_release(void)
{
	if (fw_entry) {
		release_firmware(fw_entry);
	}

	fw_entry = NULL;
}

/*******************************************************
Description:
	Novatek touchscreen request update firmware function.

return:
	Executive outcomes. 0---succeed. -1,-22---failed.
*******************************************************/
static int32_t update_firmware_request(const char *filename)
{
	uint8_t retry = 0;
	int32_t ret = 0;

	if (NULL == filename) {
		return -ENOENT;
	}

	while (1) {
		NVT_LOG("filename is %s\n", filename);

		ret = request_firmware(&fw_entry, filename, &ts->client->dev);
		if (ret) {
			NVT_ERR("firmware load failed, ret=%d\n", ret);
			goto request_fail;
		}

		/* check FW need to write size */
		if (nvt_get_fw_need_write_size(fw_entry)) {
			NVT_ERR("get fw need to write size fail!\n");
			ret = -EINVAL;
			goto invalid;
		}

		/* check if FW version add FW version bar equals 0xFF */
		if (*(fw_entry->data + FW_BIN_VER_OFFSET) + *(fw_entry->data + FW_BIN_VER_BAR_OFFSET) != 0xFF) {
			NVT_ERR("bin file FW_VER + FW_VER_BAR should be 0xFF!\n");
			NVT_ERR("FW_VER=0x%02X, FW_VER_BAR=0x%02X\n", *(fw_entry->data+FW_BIN_VER_OFFSET), *(fw_entry->data+FW_BIN_VER_BAR_OFFSET));
			ret = -ENOEXEC;
			goto invalid;
		}

		/* BIN Header Parser */
		ret = nvt_bin_header_parser(fw_entry->data, fw_entry->size);
		if (ret) {
			NVT_ERR("bin header parser failed\n");
			goto invalid;
		} else {
			break;
		}

invalid:
		update_firmware_release();
		if (!IS_ERR_OR_NULL(bin_map)) {
			kfree(bin_map);
			bin_map = NULL;
		}

request_fail:
		retry++;
		if(unlikely(retry > 2)) {
			NVT_ERR("error, retry=%d\n", retry);
			break;
		}
	}

	return ret;
}

#if NVT_DUMP_PARTITION
/*******************************************************
Description:
	Novatek touchscreen dump flash partition function.

return:
	n.a.
*******************************************************/
loff_t file_offset = 0;
static int32_t nvt_read_ram_and_save_file(uint32_t addr, uint16_t len, char *name)
{
	char file[256] = "";
	uint8_t *fbufp = NULL;
	int32_t ret = 0;
	struct file *fp = NULL;
	mm_segment_t org_fs;

	sprintf(file, "%s/dump_%s.bin", NVT_DUMP_PARTITION_PATH, name);
	NVT_LOG("Dump [%s] from 0x%08X to 0x%08X\n", file, addr, addr+len);

	fbufp = (uint8_t *)kzalloc(len+1, GFP_KERNEL);
	if(fbufp == NULL) {
		NVT_ERR("kzalloc for fbufp failed!\n");
		ret = -ENOMEM;
		goto alloc_buf_fail;
	}

	org_fs = get_fs();
	set_fs(KERNEL_DS);
	fp = filp_open(file, O_RDWR | O_CREAT, 0644);
	if (fp == NULL || IS_ERR(fp)) {
		ret = -ENOMEM;
		NVT_ERR("open file failed\n");
		goto open_file_fail;
	}

	/* SPI read */
	/* ---set xdata index to addr--- */
	nvt_set_page(addr);

	fbufp[0] = addr & 0x7F;
	CTP_SPI_READ(ts->client, fbufp, len+1);

	/* Write to file */
	ret = vfs_write(fp, (char __user *)fbufp+1, len, &file_offset);
	if (ret != len) {
		NVT_ERR("write file failed\n");
		goto open_file_fail;
	} else {
		ret = 0;
	}

open_file_fail:
	set_fs(org_fs);
	if (!IS_ERR_OR_NULL(fp)) {
		filp_close(fp, NULL);
		fp = NULL;
	}

	if (!IS_ERR_OR_NULL(fbufp)) {
		kfree(fbufp);
		fbufp = NULL;
	}
alloc_buf_fail:

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen nvt_dump_partition function to dump
 each partition for debug.

return:
	n.a.
*******************************************************/
static int32_t nvt_dump_partition(void)
{
	uint32_t list = 0;
	char *name;
	uint32_t SRAM_addr, size;
	uint32_t i = 0;
	uint16_t len = 0;
	int32_t count = 0;
	int32_t ret = 0;

	if (NVT_DUMP_PARTITION_LEN >= sizeof(ts->rbuf)) {
		NVT_ERR("dump len %d is larger than buffer size %ld\n",
				NVT_DUMP_PARTITION_LEN, sizeof(ts->rbuf));
		return -EINVAL;
	} else if (NVT_DUMP_PARTITION_LEN >= NVT_TRANSFER_LEN) {
		NVT_ERR("dump len %d is larger than NVT_TRANSFER_LEN\n", NVT_DUMP_PARTITION_LEN);
		return -EINVAL;
	}

	if (bin_map == NULL) {
		NVT_ERR("bin_map is NULL\n");
		return -ENOMEM;
	}

	memset(fwbuf, 0, (NVT_DUMP_PARTITION_LEN+1));

	for (list = 0; list < partition; list++) {
		/* initialize variable */
		SRAM_addr = bin_map[list].SRAM_addr;
		size = bin_map[list].size;
		name = bin_map[list].name;

		/* ignore reserved partition (Reserved Partition size is zero) */
		if (!size)
			continue;
		else
			size = size +1;

		/* write data to SRAM */
		if (size % NVT_DUMP_PARTITION_LEN)
			count = (size / NVT_DUMP_PARTITION_LEN) + 1;
		else
			count = (size / NVT_DUMP_PARTITION_LEN);

		for (i = 0 ; i < count ; i++) {
			len = (size < NVT_DUMP_PARTITION_LEN) ? size : NVT_DUMP_PARTITION_LEN;

			/* dump for debug download firmware */
			ret = nvt_read_ram_and_save_file(SRAM_addr, len, name);
			if (ret < 0) {
				NVT_ERR("nvt_read_ram_and_save_file failed, ret = %d\n", ret);
				goto out;
			}

			SRAM_addr += NVT_DUMP_PARTITION_LEN;
			size -= NVT_DUMP_PARTITION_LEN;
		}

		file_offset = 0;
	}

out:
	return ret;
}
#endif /* NVT_DUMP_PARTITION */

/*******************************************************
Description:
	Novatek touchscreen write data to sram function.

- fwdata   : The buffer is written
- SRAM_addr: The sram destination address
- size     : Number of data bytes in @fwdata being written
- BIN_addr : The transferred data offset of @fwdata

return:
	Executive outcomes. 0---succeed. else---fail.
*******************************************************/
static int32_t nvt_write_sram(const u8 *fwdata,
		uint32_t SRAM_addr, uint32_t size, uint32_t BIN_addr)
{
	int32_t ret = 0;
	uint32_t i = 0;
	uint16_t len = 0;
	int32_t count = 0;

	if (size % NVT_TRANSFER_LEN)
		count = (size / NVT_TRANSFER_LEN) + 1;
	else
		count = (size / NVT_TRANSFER_LEN);

	for (i = 0 ; i < count ; i++) {
		len = (size < NVT_TRANSFER_LEN) ? size : NVT_TRANSFER_LEN;

		/* ---set xdata index to start address of SRAM--- */
		ret = nvt_set_page(SRAM_addr);
		if (ret) {
			NVT_ERR("set page failed, ret = %d\n", ret);
			return ret;
		}

		/* ---write data into SRAM (page-boundary safe)--- */
		/*
		 * ★ 页边界安全写入（2026-09-14 实测）：
		 * IC 的 SPI 写引擎以 0x80 字节为一页，**跨页的传输会被整体丢弃**
		 * （实测：在 SRAM 0x11417F 写 2 字节，IC 完全没有写入）。
		 * ILM(0x0)/DLM(0x100000) 的块起点天然 0x80 对齐，所以 0xFC00 大块
		 * 传输一直是对的（已与固件逐字节比对通过）；
		 * 但 info 表末项 "Header" 的目标是 SRAM 0x114178（low7=0x78），
		 * 那一笔 0x101 字节传输必然跨页 —— 现场实测它最终落到了 +0x40。
		 * 这里把每笔传输裁到当前页内。
		 */
		{
			uint32_t done = 0;

			while (done < len) {
				uint32_t space = 0x80 - ((SRAM_addr + done) & 0x7F);
				uint32_t piece = len - done;

				if (piece > space)
					piece = space;

				ret = nvt_set_page(SRAM_addr + done);
				if (ret) {
					NVT_ERR("set page failed, ret = %d\n", ret);
					return ret;
				}
				fwbuf[0] = (SRAM_addr + done) & 0x7F;
				memcpy(fwbuf + 1, &fwdata[BIN_addr + done], piece);
				ret = CTP_SPI_WRITE(ts->client, fwbuf, piece + 1);
				if (ret) {
					NVT_ERR("write to sram failed, ret = %d\n", ret);
					return ret;
				}
				done += piece;
			}
		}
		/*
		 * 逐笔回读校验 —— 纯调试用。开着它会让每笔 SRAM 写入都多一次 SPI 读
		 * （240KB 固件 ≈ 多几百次事务）并且刷屏，量产/日常必须关。
		 *   开启： nt36xxx_ts.fwu_verify=1
		 */
		if (nvt_fwu_verify) {
			/*
			 * 整块逐字节回读比对（0x40 对齐 —— 见 0x80 字节页内回绕规则）。
			 * 只在这一层报告「差多少字节 / 第一个差异在哪」，
			 * 用于判定 IC SRAM 里的固件是否与文件逐字节相同。
			 */
			uint32_t k, bad = 0, first = 0xFFFFFFFF;
			uint8_t vb[0x81];

			for (k = 0; k < len; k += 0x40) {
				uint32_t piece = (len - k) > 0x40 ? 0x40 : (len - k);
				uint32_t a = SRAM_addr + k;
				uint32_t j;

				memset(vb, 0, sizeof(vb));
				nvt_set_page(a);
				vb[0] = a & 0x7F;
				if (CTP_SPI_READ(ts->client, vb, piece + 1)) {
					NVT_ERR("[fwu-verify] read failed at 0x%06X\n", a);
					break;
				}
				for (j = 0; j < piece; j++) {
					if (vb[1 + j] != fwdata[BIN_addr + k + j]) {
						if (first == 0xFFFFFFFF)
							first = k + j;
						bad++;
					}
				}
			}
			NVT_ERR("[fwu-verify] SRAM 0x%06X len=0x%X: %u bytes differ (first at +0x%s%X)\n",
				SRAM_addr, len, bad,
				(first == 0xFFFFFFFF) ? "0" : "", first == 0xFFFFFFFF ? 0 : first);
		}

		SRAM_addr += NVT_TRANSFER_LEN;
		BIN_addr += NVT_TRANSFER_LEN;
		size -= NVT_TRANSFER_LEN;
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen nvt_write_firmware function to write
firmware into each partition.

return:
	n.a.
*******************************************************/
static int32_t nvt_write_firmware(const u8 *fwdata, size_t fwsize)
{
	uint32_t list = 0;
	char *name;
	uint32_t BIN_addr, SRAM_addr, size;
	int32_t ret = 0;

	memset(fwbuf, 0, (NVT_TRANSFER_LEN+1));

	for (list = 0; list < partition; list++) {
		/* initialize variable */
		SRAM_addr = bin_map[list].SRAM_addr;
		size = bin_map[list].size;
		BIN_addr = bin_map[list].BIN_addr;
		name = bin_map[list].name;

	/* NVT_LOG("[%d][%s] SRAM (0x%08X), SIZE (0x%08X), BIN (0x%08X)\n",
			list, name, SRAM_addr, size, BIN_addr); */

		/* Check data size */
		if ((BIN_addr + size) > fwsize) {
			NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
					BIN_addr, BIN_addr + size);
			ret = -EINVAL;
			goto out;
		}

		/* ignore reserved partition (Reserved Partition size is zero) */
		if (!size)
			continue;
		else
			size = size +1;

		/* write data to SRAM */
		ret = nvt_write_sram(fwdata, SRAM_addr, size, BIN_addr);
		if (ret) {
			NVT_ERR("sram program failed, ret = %d\n", ret);
			goto out;
		}
	}

out:
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen check checksum function.
This function will compare file checksum and fw checksum.

return:
	n.a.
*******************************************************/
static int32_t nvt_check_fw_checksum(void)
{
	uint32_t fw_checksum = 0;
	uint32_t len = partition*4;
	uint32_t list = 0;
	int32_t ret = 0;

	memset(fwbuf, 0, (len+1));

	/* ---set xdata index to checksum--- */
	nvt_set_page(ts->mmap->R_ILM_CHECKSUM_ADDR);

	/* read checksum */
	fwbuf[0] = (ts->mmap->R_ILM_CHECKSUM_ADDR) & 0x7F;
	ret = CTP_SPI_READ(ts->client, fwbuf, len+1);
	if (ret) {
		NVT_ERR("Read fw checksum failed\n");
		return ret;
	}

	/*
	 * Compare each checksum from fw
	 * ILM + DLM + Overlay + Info
	 * ilm_dlm_num (ILM & DLM) + ovly_sec_num + info_sec_num
	 */
	for (list = 0; list < partition; list++) {
		fw_checksum = byte_to_word(&fwbuf[1+list*4]);

		/* ignore reserved partition (Reserved Partition size is zero) */
		if(!bin_map[list].size)
			continue;

		if (bin_map[list].crc != fw_checksum) {
			NVT_ERR("[%d] BIN_checksum=0x%08X, FW_checksum=0x%08X\n",
					list, bin_map[list].crc, fw_checksum);
			ret = -EIO;
		}
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen set bootload crc reg bank function.
This function will set hw crc reg before enable crc function.

return:
	n.a.
*******************************************************/
static void nvt_tx_auto_copy_mode(void)
{
	if (ts->carrier_system == 1) {
		nvt_write_addr(ts->mmap->CP_TP_CPU_REQ, 0x69);
	} else if (ts->carrier_system == 2) {
		nvt_write_addr(ts->mmap->CP_TP_CPU_REQ, 0x56);
	}
	NVT_LOG("tx auto copy mode %d enable\n", ts->carrier_system);
}

static int32_t nvt_check_tx_auto_copy(void)
{
	int32_t i = 0;
	uint8_t buf[4] = {0};
	int32_t retry = 200;

	if (ts->mmap->CP_TP_CPU_REQ == 0) {
		NVT_ERR("error, TX_AUTO_COPY_EN = 0\n");
		return -1;
	}

	for (i = 0; i < retry; i++) {
		nvt_set_page(ts->mmap->CP_TP_CPU_REQ);
		buf[0] = ts->mmap->CP_TP_CPU_REQ & 0x7F;
		buf[1] = 0xFF;
		CTP_SPI_READ(ts->client, buf, 2);

		if (buf[1] == 0x00) {
			NVT_LOG("tx auto copy done (i=%d)!\n", i);
			return 0;
		}

		usleep_range(1000, 1000);
	}

	NVT_ERR("tx auto copy timeout! i=%d, buf[1]=0x%02X\n", i, buf[1]);
	return -ETIMEDOUT;
}

static int32_t nvt_wait_auto_copy(void)
{
	int32_t ret = 0;

	if (ts->carrier_system == 2) {
		ret = nvt_check_tx_auto_copy();
	} else if (ts->carrier_system == 1) {
		ret = 0;
	} else {
		NVT_ERR("failed, not support mode %d!\n", ts->carrier_system);
		ret = -1;
	}

	return ret;
}

static void nvt_set_bld_crc_bank(uint32_t DES_ADDR, uint32_t SRAM_ADDR,
		uint32_t LENGTH_ADDR, uint32_t size,
		uint32_t G_CHECKSUM_ADDR, uint32_t crc)
{
	/* write destination address */
	nvt_set_page(DES_ADDR);
	fwbuf[0] = DES_ADDR & 0x7F;
	fwbuf[1] = (SRAM_ADDR) & 0xFF;
	fwbuf[2] = (SRAM_ADDR >> 8) & 0xFF;
	fwbuf[3] = (SRAM_ADDR >> 16) & 0xFF;
	CTP_SPI_WRITE(ts->client, fwbuf, 4);

	/* write length */
	nvt_set_page(LENGTH_ADDR);
	fwbuf[0] = LENGTH_ADDR & 0x7F;
	fwbuf[1] = (size) & 0xFF;
	fwbuf[2] = (size >> 8) & 0xFF;
	fwbuf[3] = (size >> 16) & 0xFF;
	if (ts->hw_crc == 1) {
		CTP_SPI_WRITE(ts->client, fwbuf, 3);
	} else if (ts->hw_crc > 1) {
		CTP_SPI_WRITE(ts->client, fwbuf, 4);
	}

	/* write golden checksum */
	nvt_set_page(G_CHECKSUM_ADDR);
	fwbuf[0] = G_CHECKSUM_ADDR & 0x7F;
	fwbuf[1] = (crc) & 0xFF;
	fwbuf[2] = (crc >> 8) & 0xFF;
	fwbuf[3] = (crc >> 16) & 0xFF;
	fwbuf[4] = (crc >> 24) & 0xFF;
	CTP_SPI_WRITE(ts->client, fwbuf, 5);

	return;
}

/*******************************************************
Description:
	Novatek touchscreen set BLD hw crc function.
This function will set ILM and DLM crc information to register.

return:
	n.a.
*******************************************************/
static void nvt_set_bld_hw_crc(void)
{
	/* [0] ILM */
	/* write register bank */
	nvt_set_bld_crc_bank(ts->mmap->ILM_DES_ADDR, bin_map[0].SRAM_addr,
			ts->mmap->ILM_LENGTH_ADDR, bin_map[0].size,
			ts->mmap->G_ILM_CHECKSUM_ADDR, bin_map[0].crc);

	/* [1] DLM */
	/* write register bank */
	nvt_set_bld_crc_bank(ts->mmap->DLM_DES_ADDR, bin_map[1].SRAM_addr,
			ts->mmap->DLM_LENGTH_ADDR, bin_map[1].size,
			ts->mmap->G_DLM_CHECKSUM_ADDR, bin_map[1].crc);
}

/*******************************************************
Description:
	Novatek touchscreen read BLD hw crc info function.
This function will check crc results from register.

return:
	n.a.
*******************************************************/
static void nvt_read_bld_hw_crc(void)
{
	uint8_t buf[8] = {0};
	uint32_t g_crc = 0, r_crc = 0;

	/* CRC Flag */
	nvt_set_page(ts->mmap->BLD_ILM_DLM_CRC_ADDR);
	buf[0] = ts->mmap->BLD_ILM_DLM_CRC_ADDR & 0x7F;
	buf[1] = 0x00;
	CTP_SPI_READ(ts->client, buf, 2);
	NVT_ERR("crc_done = %d, ilm_crc_flag = %d, dlm_crc_flag = %d\n",
			(buf[1] >> 2) & 0x01, (buf[1] >> 0) & 0x01, (buf[1] >> 1) & 0x01);

	/* ILM CRC */
	nvt_set_page(ts->mmap->G_ILM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_ILM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	g_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	nvt_set_page(ts->mmap->R_ILM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->R_ILM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	r_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	NVT_ERR("ilm: bin crc = 0x%08X, golden = 0x%08X, result = 0x%08X\n",
			bin_map[0].crc, g_crc, r_crc);

	/* DLM CRC */
	nvt_set_page(ts->mmap->G_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_DLM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	g_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	nvt_set_page(ts->mmap->R_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->R_DLM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	r_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	NVT_ERR("dlm: bin crc = 0x%08X, golden = 0x%08X, result = 0x%08X\n",
			bin_map[1].crc, g_crc, r_crc);

	return;
}

/* ---------------------------------------------------------------------------
 * Diagnostic: dump the NT36532 BLD-CRC register bank (0x1FB500..0x1FB53F).
 * Added 2026-09-14 to trace the hw_crc download flow.  Purely read-only.
 * ------------------------------------------------------------------------- */
void nvt_dump_bld_bank(const char *tag)
{
	uint8_t buf[66] = {0};
	uint32_t g_ilm, g_dlm, r_ilm, r_dlm;
	uint32_t bld_des, ilm_des, dlm_des, ilm_len, dlm_len, bld_len;
	int32_t ret;
	int i;

	nvt_set_page(BLD_BANK_ADDR);
	buf[0] = (uint8_t)(BLD_BANK_ADDR & 0x7F);
	ret = CTP_SPI_READ(ts->client, buf, 65);
	if (ret) {
		NVT_ERR("[bld:%s] SPI read failed (%d)\n", tag, ret);
		return;
	}

	NVT_ERR("[bld:%s] raw 0x1FB500..0x1FB53F:\n", tag);
	for (i = 0; i < 64; i += 16) {
		NVT_ERR("  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
			buf[1 + i + 0], buf[1 + i + 1], buf[1 + i + 2], buf[1 + i + 3],
			buf[1 + i + 4], buf[1 + i + 5], buf[1 + i + 6], buf[1 + i + 7],
			buf[1 + i + 8], buf[1 + i + 9], buf[1 + i + 10], buf[1 + i + 11],
			buf[1 + i + 12], buf[1 + i + 13], buf[1 + i + 14], buf[1 + i + 15]);
	}

	g_ilm   = RD32(0x00); g_dlm   = RD32(0x04);
	r_ilm   = RD32(0x20); r_dlm   = RD32(0x24);
	bld_des = RD24(0x14); ilm_des = RD24(0x28); dlm_des = RD24(0x2C);
	ilm_len = RD24(0x18); dlm_len = RD24(0x30); bld_len = RD24(0x38);

	NVT_ERR("[bld:%s] G_ILM=0x%08X G_DLM=0x%08X | R_ILM=0x%08X R_DLM=0x%08X\n",
		tag, g_ilm, g_dlm, r_ilm, r_dlm);
	NVT_ERR("[bld:%s] DES bld=0x%06X ilm=0x%06X dlm=0x%06X | LEN ilm=0x%06X dlm=0x%06X bld=0x%06X\n",
		tag, bld_des, ilm_des, dlm_des, ilm_len, dlm_len, bld_len);
	NVT_ERR("[bld:%s] BOOT_RDY=0x%02X ILMDLM_CRC=0x%02X DMA_CRC_FLAG=0x%02X BLD_CRC_EN=0x%02X"
		" w508=%02X%02X%02X%02X w50C=%02X%02X%02X%02X w510=%02X%02X%02X%02X w51C=%02X%02X%02X%02X\n",
		tag, buf[1 + 0x0D], buf[1 + 0x33], buf[1 + 0x34], buf[1 + 0x36],
		buf[1 + 0x0B], buf[1 + 0x0A], buf[1 + 0x09], buf[1 + 0x08],
		buf[1 + 0x0F], buf[1 + 0x0E], buf[1 + 0x0D], buf[1 + 0x0C],
		buf[1 + 0x13], buf[1 + 0x12], buf[1 + 0x11], buf[1 + 0x10],
		buf[1 + 0x1F], buf[1 + 0x1E], buf[1 + 0x1D], buf[1 + 0x1C]);

	/* 2026-09-14: 0x1FC900 looks like a SECOND copy of the same BLD layout
	 * (peer / slave of the cascade pair).  Dump it alongside so the two
	 * halves of the negotiation can be compared. */
	{
		uint8_t alt[66] = {0};
		int k;
		nvt_set_page(0x1FC900);
		alt[0] = 0x1FC900 & 0x7F;
		if (CTP_SPI_READ(ts->client, alt, 65) == 0) {
			for (k = 0; k < 64; k += 16) {
				NVT_ERR("[bld:%s] peer 0x1FC9%02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
					tag, k,
					alt[1+k+0], alt[1+k+1], alt[1+k+2], alt[1+k+3],
					alt[1+k+4], alt[1+k+5], alt[1+k+6], alt[1+k+7],
					alt[1+k+8], alt[1+k+9], alt[1+k+10], alt[1+k+11],
					alt[1+k+12], alt[1+k+13], alt[1+k+14], alt[1+k+15]);
			}
		} else {
			NVT_ERR("[bld:%s] peer bank 0x1FC900 read failed\n", tag);
		}
	}

	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);
}

#if NVT_TOUCH_ESD_DISP_RECOVERY
#define ILM_CRC_FLAG 0x01
#define DLM_CRC_FLAG 0x02
#define CRC_DONE 0x04
static int32_t nvt_check_crc_done_ilm_err(void)
{
	uint8_t buf[8] = {0};

	nvt_set_page(ts->mmap->BLD_ILM_DLM_CRC_ADDR);
	buf[0] = ts->mmap->BLD_ILM_DLM_CRC_ADDR & 0x7F;
	buf[1] = 0x00;
	CTP_SPI_READ(ts->client, buf, 2);

	NVT_LOG("CRC DONE, ILM DLM FLAG = 0x%02X\n", buf[1]);
	if (((buf[1] & ILM_CRC_FLAG) && (buf[1] & CRC_DONE)) ||
		((buf[1] & DLM_CRC_FLAG) && (buf[1] & CRC_DONE)) ||
		(buf[1] == 0xFE) || ((buf[1] & CRC_DONE) == 0x00))
		return 1;
	else
		return 0;
}

#define DISP_OFF_ADDR 0x2800
__attribute__((unused))static int nvt_f2c_disp_off(void)
{
	uint8_t buf[8] = {0};
	int ret = 0;
	uint8_t tmp_val = 0;
	int32_t write_disp_off_retry = 0;
	int32_t retry = 0;
	int32_t i = 0;

	NVT_LOG("%s ++\n", __func__);

	/* SW Reset & Idle */
	nvt_sw_reset_idle();

	/* read 0x3F01A and print */
	nvt_set_page(0x3F01A);
	buf[0] = 0x3F01A & 0x7F;
	buf[1] = 0x00;
	CTP_SPI_READ(ts->client, buf, 2);
	NVT_ERR("0x3F01A = 0x%02X\n", buf[1]);
	/* read 0x3F280 and print */
	nvt_set_page(0x3F280);
	buf[0] = 0x3F280 & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	CTP_SPI_READ(ts->client, buf, 3);
	NVT_ERR("0x3F280 = 0x%02X, 0x3F281 = 0x%02X\n", buf[1], buf[2]);

	/* Setp1: Set REG CPU_IF_ADDR[15:0] */
	nvt_write_addr(ts->mmap->CPU_IF_ADDR_LOW, DISP_OFF_ADDR & 0xFF);
	nvt_write_addr(ts->mmap->CPU_IF_ADDR_HIGH, (DISP_OFF_ADDR >> 8) & 0xFF);

	/* Step2: Set REG FFM_ADDR[15:0] */
	/* set FFM_ADDR to 0x20000 */
	nvt_write_addr(ts->mmap->FFM_ADDR_LOW, 0x00);
	nvt_write_addr(ts->mmap->FFM_ADDR_MID, 0x00);
	if (ts->hw_crc > 1)
		nvt_write_addr(ts->mmap->FFM_ADDR_HIGH, 0x00);

	/* Step3: Set REG F2C_LENGT[H7:0] */
	nvt_write_addr(ts->mmap->F2C_LENGTH, 1);

	/* Enable CP_TP_CPU_REQ */
	nvt_write_addr(ts->mmap->CP_TP_CPU_REQ, 0x01);

nvt_write_disp_off_retry:
	/* Step4: Set REG CPU_Polling_En=1, F2C_RW=1, CPU_IF_ADDR_INC=1, F2C_EN=1 */
	nvt_set_page(ts->mmap->FFM2CPU_CTL);
	buf[0] = ts->mmap->FFM2CPU_CTL & 0x7F;
	buf[1] = 0xFF;
	ret = CTP_SPI_READ(ts->client, buf, 2);
	if (ret) {
		NVT_ERR("Read FFM2CPU control failed!\n");
		return ret;
	}
	tmp_val = buf[1] | 0x27;
	nvt_write_addr(ts->mmap->FFM2CPU_CTL, tmp_val);

	/* Step5: wait F2C_EN = 0 */
	retry = 0;
	while (1) {
		nvt_set_page(ts->mmap->FFM2CPU_CTL);
		buf[0] = ts->mmap->FFM2CPU_CTL & 0x7F;
		buf[1] = 0xFF;
		buf[2] = 0xFF;
		ret = CTP_SPI_READ(ts->client, buf, 3);
		if (ret) {
			NVT_ERR("Read FFM2CPU control failed!\n");
			return ret;
		}

		if ((buf[1] & 0x01) == 0x00)
			break;

		usleep_range(1000, 1000);
		retry++;

		if(unlikely(retry > 40)) {
			NVT_ERR("Wait F2C_EN = 0 failed! retry = %d\n", retry);

			for (i = 0; i < 3; i++) {
				nvt_set_page(0x3F000);
				buf[0] = 0x3F000 & 0x7F;
				buf[1] = 0x00;
				buf[2] = 0x00;
				buf[3] = 0x00;
				buf[4] = 0x00;
				buf[5] = 0x00;
				CTP_SPI_READ(ts->client, buf, 6);
				NVT_ERR("0x3F000 ~ 0x3F004 = 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
					buf[1], buf[2], buf[3], buf[4], buf[5]);
			}
			/* read 0x3F01A and print */
			nvt_set_page(0x3F01A);
			buf[0] = 0x3F01A & 0x7F;
			buf[1] = 0x00;
			CTP_SPI_READ(ts->client, buf, 2);
			NVT_ERR("0x3F01A = 0x%02X\n", buf[1]);
			/* read 0x3F280 and print */
			nvt_set_page(0x3F280);
			buf[0] = 0x3F280 & 0x7F;
			buf[1] = 0x00;
			buf[2] = 0x00;
			CTP_SPI_READ(ts->client, buf, 3);
			NVT_ERR("0x3F280 = 0x%02X, 0x3F281 = 0x%02X\n", buf[1], buf[2]);

			NVT_ERR("set g_trigger_disp_esd_recovery true!\n");
			//if (g_esd_ctx->panel_init) {
			//	atomic_set(&g_esd_ctx->ext_te_event, 1);
			//	wake_up_interruptible(&g_esd_ctx->ext_te_wq);
			//}

			return -EIO;
		}
	}

	/* Step6: Check REG TH_CPU_CHK  status (1: Success,  0: Fail), if 0, can Retry Step4. */
	if (((buf[2] & 0x04) >> 2) != 0x01) {
		write_disp_off_retry++;
		if (write_disp_off_retry <= 5) {
			goto nvt_write_disp_off_retry;
		} else {
			NVT_ERR("Write display off failed!, buf[1]=0x%02X, buf[2]=0x%02X\n", buf[1], buf[2]);
			for (i = 0; i < 3; i++) {
				nvt_set_page(0x3F000);
				buf[0] = 0x3F000 & 0x7F;
				buf[1] = 0x00;
				buf[2] = 0x00;
				buf[3] = 0x00;
				buf[4] = 0x00;
				buf[5] = 0x00;
				CTP_SPI_READ(ts->client, buf, 6);
				NVT_ERR("0x3F000 ~ 0x3F004 = 0x%02X 0x%02X 0x%02X 0x%02X 0x02X\n",
					buf[1], buf[2], buf[3], buf[4], buf[5]);
			}
			/* read 0x3F01A and print */
			nvt_set_page(0x3F01A);
			buf[0] = 0x3F01A & 0x7F;
			buf[1] = 0x00;
			CTP_SPI_READ(ts->client, buf, 2);
			NVT_ERR("0x3F01A = 0x%02X\n", buf[1]);
			/* read 0x3F280 and print */
			nvt_set_page(0x3F280);
			buf[0] = 0x3F280 & 0x7F;
			buf[1] = 0x00;
			buf[2] = 0x00;
			CTP_SPI_READ(ts->client, buf, 3);
			NVT_ERR("0x3F280 = 0x%02X, 0x3F281 = 0x%02X\n", buf[1], buf[2]);
			//if (g_esd_ctx->panel_init) {
			//	atomic_set(&g_esd_ctx->ext_te_event, 1);
			//	wake_up_interruptible(&g_esd_ctx->ext_te_wq);
			//}
			return -EIO;
		}
	}
	NVT_LOG("%s --\n", __func__);

	return ret;
}
#endif /* #if NVT_TOUCH_ESD_DISP_RECOVERY */


/*******************************************************
Description:
	Novatek touchscreen Download_Firmware with HW CRC
function. It's complete download firmware flow.

return:
	Executive outcomes. 0---succeed. else---fail.
*******************************************************/
/*******************************************************
Description:
	把 EVENT_BUF 前 0x80 字节全量 dump 出来（0x80 页内，安全）。
	用于在 reset_state 卡住时寻找隐藏的状态位。
*******************************************************/
static void nvt_dump_event_buf(const char *tag)
{
	uint8_t buf[0x81];
	int i;

	memset(buf, 0, sizeof(buf));
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);
	buf[0] = ts->mmap->EVENT_BUF_ADDR & 0x7F;
	if (CTP_SPI_READ(ts->client, buf, 0x81)) {
		NVT_ERR("[evbuf:%s] read failed\n", tag);
		return;
	}
	for (i = 0; i < 0x80; i += 16) {
		NVT_ERR("[evbuf:%s] +%02X: %02X %02X %02X %02X %02X %02X %02X %02X  "
			"%02X %02X %02X %02X %02X %02X %02X %02X\n", tag, i,
			buf[1+i+0], buf[1+i+1], buf[1+i+2], buf[1+i+3],
			buf[1+i+4], buf[1+i+5], buf[1+i+6], buf[1+i+7],
			buf[1+i+8], buf[1+i+9], buf[1+i+10], buf[1+i+11],
			buf[1+i+12], buf[1+i+13], buf[1+i+14], buf[1+i+15]);
	}
}

/*******************************************************
Description:
	决定性实验（只在首次失败路径调用一次）：
	  1. 读出 IC 自己算出的 R_DLM
	  2. 以它作为 golden 重跑一遍完整 hw_crc 下载
	  3. 再查 reset_state

	若第 3 步通过 ⇒ 「DLM golden 与 IC 计算结果不匹配」就是启动门；
	若仍不过 ⇒ DLM CRC 不是启动门，问题在环境（显示/TDDI 等）。
*******************************************************/
static void nvt_dlm_golden_retry(void)
{
	uint8_t buf[16] = {0};
	uint32_t g_ilm, g_dlm, r_ilm, r_dlm, orig;
	int32_t ret;

	memset(buf, 0, sizeof(buf));
	nvt_set_page(BLD_BANK_ADDR);
	buf[0] = BLD_BANK_ADDR & 0x7F;
	if (CTP_SPI_READ(ts->client, buf, 9)) {
		NVT_ERR("[dlm-retry] bank read failed\n");
		return;
	}
	g_ilm = RD32(0x00);
	g_dlm = RD32(0x04);
	r_ilm = RD32(0x20);
	r_dlm = RD32(0x24);

	NVT_ERR("[dlm-retry] bank now: G_ILM=0x%08X R_ILM=0x%08X | G_DLM=0x%08X R_DLM=0x%08X\n",
		g_ilm, r_ilm, g_dlm, r_dlm);
	NVT_ERR("[dlm-retry] bin: ILM crc=0x%08X DLM crc=0x%08X  (ILMDLM_CRC=0x%02X)\n",
		bin_map[0].crc, bin_map[1].crc, buf[1 + 0x33]);

	nvt_dump_event_buf("after-fail");

	orig = bin_map[1].crc;
	bin_map[1].crc = r_dlm;

	NVT_ERR("[dlm-retry] re-running download with G_DLM := R_DLM = 0x%08X\n", r_dlm);

	nvt_bootloader_reset();
	nvt_set_bld_hw_crc();
	nvt_tx_auto_copy_mode();
	ret = nvt_write_firmware(fw_entry->data, fw_entry->size);
	if (ret) {
		NVT_ERR("[dlm-retry] write failed (%d)\n", ret);
		goto out;
	}
	ret = nvt_wait_auto_copy();
	if (ret) {
		NVT_ERR("[dlm-retry] autocopy failed (%d)\n", ret);
		goto out;
	}
	nvt_fw_crc_enable();
	nvt_boot_ready();

	ret = nvt_check_fw_reset_state(RESET_STATE_INIT);
	NVT_ERR("[dlm-retry] RESULT: check_fw_reset_state(INIT) = %d\n", ret);
	if (!ret) {
		nvt_dump_event_buf("dlm-retry-OK");
		nvt_change_mode(0);
		nvt_check_fw_reset_state(RESET_STATE_NORMAL_RUN);
	} else {
		memset(buf, 0, sizeof(buf));
		nvt_set_page(BLD_BANK_ADDR);
		buf[0] = BLD_BANK_ADDR & 0x7F;
		CTP_SPI_READ(ts->client, buf, 9);
		NVT_ERR("[dlm-retry] bank after: G_DLM=0x%08X R_DLM=0x%08X ILMDLM_CRC=0x%02X\n",
			RD32(0x04), RD32(0x24), buf[1 + 0x33]);
	}

out:
	bin_map[1].crc = orig;
	return;
}

static int32_t nvt_download_firmware_hw_crc(void)
{
	uint8_t retry = 0;
	int32_t ret = 0;

	start = ktime_get();

	while (1) {
		/* vendor-faithful: no reset per retry (vendor relies on the single probe-level reset) */

		/* bootloader reset to reset MCU */
		nvt_bootloader_reset();
		nvt_dump_bld_bank("1-after-bootloader-reset");

		/* set ilm & dlm reg bank (MUST precede write_firmware as in vendor nt36532.ko) */
		nvt_set_bld_hw_crc();
		nvt_dump_bld_bank("2-after-set-bld-hw-crc");

		/* cascade tx auto copy mode */
		if (nvt_no_autocopy)
			NVT_ERR("[noautocopy] skipping tx_auto_copy_mode\n");
		else
			nvt_tx_auto_copy_mode();

		/* Start to write firmware process */
		ret = nvt_write_firmware(fw_entry->data, fw_entry->size);
		if (ret) {
			NVT_ERR("Write_Firmware failed. (%d)\n", ret);
			goto fail;
		}

		/* wait for auto copy to complete */
		ret = nvt_no_autocopy ? 0 : nvt_wait_auto_copy();
		if (ret) {
			NVT_ERR("nvt_wait_auto_copy failed. (%d)\n", ret);
			goto fail;
		}
		nvt_dump_bld_bank("3-after-write+autocopy");

#if NVT_DUMP_PARTITION
		ret = nvt_dump_partition();
		if (ret) {
			NVT_ERR("nvt_dump_partition failed, ret = %d\n", ret);
		}
#endif

		/* clear fw reset status & enable fw crc check */
		nvt_fw_crc_enable();
		nvt_dump_bld_bank("4-after-fw-crc-enable");

		/* Set Boot Ready Bit */
		nvt_boot_ready();
		nvt_dump_bld_bank("5-after-boot-ready");

		ret = nvt_check_fw_reset_state(RESET_STATE_INIT);
		if (ret) {
			NVT_ERR("nvt_check_fw_reset_state failed. (%d)\n", ret);
			/* ★ 决定性实验：只在第一轮做一次，之后按原逻辑重试 */
			if (retry == 0) {
				nvt_dlm_golden_retry();
				ret = nvt_check_fw_reset_state(RESET_STATE_INIT);
				NVT_ERR("[dlm-retry] post-experiment reset_state check = %d\n", ret);
				if (!ret) {
					nvt_change_mode(0);
					nvt_check_fw_reset_state(RESET_STATE_NORMAL_RUN);
					break;
				}
			}
			goto fail;
		} else {
			nvt_change_mode(0);
			nvt_check_fw_reset_state(RESET_STATE_NORMAL_RUN);
			break;
		}

fail:
		retry++;
		if(unlikely(retry > 2)) {
			NVT_ERR("error, retry=%d\n", retry);
			nvt_read_bld_hw_crc();
#if NVT_TOUCH_ESD_DISP_RECOVERY
			if (nvt_check_crc_done_ilm_err()) {
				NVT_ERR("set g_trigger_disp_esd_recovery true!\n");
				//if (g_esd_ctx->panel_init) {
				//	atomic_set(&g_esd_ctx->ext_te_event, 1);
				//	wake_up_interruptible(&g_esd_ctx->ext_te_wq);
				//}

			}
#endif /* #if NVT_TOUCH_ESD_DISP_RECOVERY */
			break;
		}
	}

	end = ktime_get();

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen Download_Firmware function. It's
complete download firmware flow.

return:
	n.a.
*******************************************************/
static int32_t nvt_download_firmware(void)
{
	uint8_t retry = 0;
	int32_t ret = 0;

	start = ktime_get();

	while (1) {
		/*
		 * Send eng reset cmd before download FW
		 * Keep TP_RESX low when send eng reset cmd
		 */
#if NVT_TOUCH_SUPPORT_HW_RST
		gpio_set_value(ts->reset_gpio, 0);
		mdelay(1);
#endif
		nvt_eng_reset();
#if NVT_TOUCH_SUPPORT_HW_RST
		gpio_set_value(ts->reset_gpio, 1);
		mdelay(10);
#endif
		nvt_bootloader_reset();

		/* clear fw reset status */
		nvt_write_addr(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE, 0x00);

		/* Start to write firmware process */
		ret = nvt_write_firmware(fw_entry->data, fw_entry->size);
		if (ret) {
			NVT_ERR("Write_Firmware failed. (%d)\n", ret);
			goto fail;
		}

#if NVT_DUMP_PARTITION
		ret = nvt_dump_partition();
		if (ret) {
			NVT_ERR("nvt_dump_partition failed, ret = %d\n", ret);
		}
#endif

		/* Set Boot Ready Bit */
		nvt_boot_ready();

		ret = nvt_check_fw_reset_state(RESET_STATE_INIT);
		if (ret) {
			NVT_ERR("nvt_check_fw_reset_state failed. (%d)\n", ret);
			goto fail;
		}

		/* check fw checksum result */
		if (!ts->hw_crc) {
			ret = nvt_check_fw_checksum();
			if (ret) {
				NVT_ERR("firmware checksum not match, retry=%d\n", retry);
				goto fail;
			}
		}
		break;

fail:
		retry++;
		if(unlikely(retry > 2)) {
			NVT_ERR("error, retry=%d\n", retry);
			break;
		}
	}

	end = ktime_get();

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen update firmware main function.

return:
	n.a.
*******************************************************/
int32_t nvt_update_firmware(const char *firmware_name)
{
	int32_t ret = 0;

	/*
	 * 唯一的擦写入口：所有路径（boot 更新 / ESD-WDT 恢复 / proc sysfs）都过这里，
	 * 所以闸门放在这个点上最稳。没有校验过芯片身份就不允许触碰 flash。
	 */
	if (!ts->fw_update_allowed) {
		NVT_ERR("chip identity NOT verified -> refusing to write flash (%s).\n",
			firmware_name);
		NVT_ERR("  set nt36xxx_ts.force_fw_update=1 only after the chip answers correctly\n");
		return -EPERM;
	}

	/* request bin file in "/etc/firmware" */
	ret = update_firmware_request(firmware_name);
	if (ret) {
		NVT_ERR("update_firmware_request failed. (%d)\n", ret);
		goto request_firmware_fail;
	}

	/* initial buffer and variable */
	ret = nvt_download_init();
	if (ret) {
		NVT_ERR("Download Init failed. (%d)\n", ret);
		goto download_fail;
	}

	if (bin_map) {
		int _i;
		for (_i = 0; _i < 2; _i++)
			NVT_ERR("[bld:0-bin-map] [%d]%s BIN=0x%06X SRAM=0x%06X size=0x%06X crc=0x%08X\n",
				_i, bin_map[_i].name, bin_map[_i].BIN_addr, bin_map[_i].SRAM_addr,
				bin_map[_i].size, bin_map[_i].crc);
	}

	/* 2026-09-14: prove the runtime map equals the vendor table */
	{
		const uint32_t *mp = (const uint32_t *)ts->mmap;
		int _k;
		for (_k = 0; _k < (int)(sizeof(*ts->mmap) / sizeof(uint32_t)); _k++)
			NVT_ERR("[mmap] [%02d] = 0x%08X\n", _k, mp[_k]);
	}

	/* download firmware process */
	if (ts->hw_crc)
		ret = nvt_download_firmware_hw_crc();
	else
		ret = nvt_download_firmware();
	if (ret) {
		NVT_ERR("Download Firmware failed. (%d)\n", ret);
		goto download_fail;
	}

	NVT_LOG("Update firmware success! <%ld us>\n",
			(long) ktime_us_delta(end, start));

	/* Get FW Info (non-fatal, matches vendor nt36532.ko line 7008/702c behavior) */
	if (nvt_get_fw_info()) {
		NVT_LOG("nvt_get_fw_info deferred / non-fatal\n");
	}
	ret = 0;

download_fail:
	if (!IS_ERR_OR_NULL(bin_map)) {
		kfree(bin_map);
		bin_map = NULL;
	}

	update_firmware_release();
request_firmware_fail:

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen update firmware when booting
	function.

return:
	n.a.
*******************************************************/
void Boot_Update_Firmware(struct work_struct *work)
{
	int32_t ret = 0;

	/*
	 * ★ 安全闸门（2026-09-12）：只有在 probe 里真正读回过有效的 chip ID 才允许
	 *   擦写 flash。否则 SPI 读回恒为 0 时也会照走完整流程 —— 包括 erase ——
	 *   把固件刷给一颗身份不明的芯片；写错的东西掉电后不会自愈。
	 */
	if (!ts->fw_update_allowed) {
		NVT_ERR("chip identity NOT verified -> skipping firmware flash "
			"(running on the IC's built-in firmware)\n");
		NVT_ERR("  set nt36xxx_ts.force_fw_update=1 only after the chip answers correctly\n");
		nvt_change_mode(0);
		nvt_check_fw_reset_state(RESET_STATE_NORMAL_RUN);
		nvt_get_fw_info();
		pm_relax(&ts->client->dev);
		return;
	}

	nvt_match_fw();
	mutex_lock(&ts->lock);
	NVT_LOG("BOOT FW update start (fw=%s)\n", ts->fw_name);
	ret = nvt_update_firmware(ts->fw_name);
	if (ret) {
		const char *alt_fw = (strcmp(ts->fw_name, DEFAULT_BOOT_UPDATE_FIRMWARE_FIRST) == 0) ?
				     DEFAULT_BOOT_UPDATE_FIRMWARE_SECOND : DEFAULT_BOOT_UPDATE_FIRMWARE_FIRST;
		const char *alt_mp = (strcmp(ts->fw_name, DEFAULT_BOOT_UPDATE_FIRMWARE_FIRST) == 0) ?
				     DEFAULT_MP_UPDATE_FIRMWARE_SECOND : DEFAULT_MP_UPDATE_FIRMWARE_FIRST;
		NVT_ERR("Update firmware %s failed (%d), trying alternate %s...\n",
			ts->fw_name, ret, alt_fw);
		ts->fw_name = alt_fw;
		ts->mp_name = alt_mp;
		ret = nvt_update_firmware(ts->fw_name);
		if (ret)
			NVT_ERR("Alternate panel firmware also failed (%d)!\n", ret);
	}
	if (ret == 0) {
		ts->fw_ready = true;
		NVT_LOG("Touch firmware update SUCCESS! Touchscreen is now ACTIVE.\n");
		nvt_change_mode(0);
		nvt_check_fw_reset_state(RESET_STATE_NORMAL_RUN);
	} else {
		static int fwu_retry_cnt;
		if (fwu_retry_cnt < 20) {
			fwu_retry_cnt++;
			NVT_LOG("Touch FW update failed (%d), will retry in 3s (attempt %d/20)...\n", ret, fwu_retry_cnt);
			queue_delayed_work(nvt_fwu_wq, &ts->nvt_fwu_work, msecs_to_jiffies(3000));
		}
	}
	nvt_get_fw_info();
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);
	NVT_LOG("BOOT FW update done, ret=%d\n", ret);
	mutex_unlock(&ts->lock);
	pm_relax(&ts->client->dev);
}
#endif /* BOOT_UPDATE_FIRMWARE */
