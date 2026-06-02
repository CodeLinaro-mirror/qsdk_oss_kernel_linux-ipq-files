/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include <linux/bitfield.h>
#include <linux/memblock.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/of_device.h>
#include <linux/devcoredump.h>
#include <linux/elf.h>
#include <linux/of_address.h>
#include "commonmhitest.h"
#include "../host/internal.h"

#define QCN9000_DEFAULT_FW_FILE_NAME	"qcn9000/amss.bin"
#define QCN9224_DEFAULT_FW_FILE_NAME	"qcn9224/amss.bin"
#define QCC2072_DEFAULT_FW_FILE_NAME	"qcc2072/amss.bin"
#define QCN9625_DEFAULT_FW_FILE_NAME	"qcn9625/amss.bin"
#define QCN9589_DEFAULT_FW_FILE_NAME	"qcn9589/amss.bin"

#define PCIE_PCIE_LOCAL_REG_PCIE_LOCAL_RSV0		0x3164
#define QCN9625_PCIE_PCIE_LOCAL_REG_PCIE_LOCAL_RSV0	0x3300
#define DOMAIN_NR_MASK					GENMASK(7, 4)
#define BUS_NR_MASK					GENMASK(3, 0)

#define MHITEST_MHI_SEG_LEN			SZ_512K
#define MHITEST_DUMP_DESC_TOLERANCE		64
#define MAX_RAMDUMP_TABLE_SIZE			6
#define COREDUMP_DESC				"Q6-COREDUMP"
#define Q6_SFR_DESC				"Q6-SFR"

#define MHISTATUS				0x48
#define MHICTRL					0x38
#define QCN9625_BHI_EXECENV				0x128

#define QCN9224_PCIE_REMAP_BAR_CTRL_OFFSET	0x310C
#define QCN9625_PCIE_REMAP_BAR_CTRL_OFFSET	0x3278
#define QCN9224_PCI_MHIREGLEN_REG		0x1E0E100
#define QCN9224_PCI_MHI_REGION_END		0x1E0EFFC
#define PCIE_LOCAL_REG_BASE			0x1E00000
#define PCIE_LOCAL_REG_END			0x1E03FFF

#define WINDOW_SHIFT				19
#define QCN9224_WINDOW_VALUE_MASK		0x3f
#define QCN9625_WINDOW_VALUE_MASK		0x7f
#define WINDOW_ENABLE_BIT			0x40000000
#define MAX_UNWINDOWED_ADDRESS			0x80000
#define WINDOW_START				MAX_UNWINDOWED_ADDRESS
#define WINDOW_RANGE_MASK			0x7FFFF

#define QCN9625_Q6_BCR_RESET			0x1E381F0

#define TIMEOUT_SAVE_DUMP_MS 300000

static DEFINE_SPINLOCK(pci_reg_window_lock);
static DEFINE_SPINLOCK(global_soc_reset_lock);

#define RDDM_LINK_RECOVERY_RETRY		20
#define RDDM_LINK_RECOVERY_RETRY_DELAY_MS	20

#define QCN9224_PCIE_EN_LEGACY_MHI			0xE000
#define QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_STS4	0x31C4
#define QCN9224_PCIE_LOCAL_REG_LEGACY_INTX_EN_CLR_BNK4	0x31B0
#define QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_COMN	0x31CC
#define QCN9224_PCIE_LOCAL_REG_LEGACY_INTX_EN_BNK4	0x3188

bool autostart = true;
module_param(autostart, bool, 0);
MODULE_PARM_DESC(autostart, "Do need to power up mhi during module load?");

bool pblsbl_debug = true;
module_param(pblsbl_debug, bool, 0);
MODULE_PARM_DESC(pblsbl_debug, "Do need to dump pbl/sbl debug logs if mhi doesn't come up?");

/* Timeout, to print boot debug logs, in seconds */
int boot_debug_timeout = 7;
module_param(boot_debug_timeout, int, 0644);
MODULE_PARM_DESC(boot_debug_timeout, "boot debug logs timeout in seconds");
#define BOOT_DEBUG_TIMEOUT_MS		(boot_debug_timeout * 1000)

int soc_reset_delay_ms = 10;
module_param(soc_reset_delay_ms, int, 0644);
MODULE_PARM_DESC(soc_reset_delay_ms, "soc reset delay in milliseconds");

/* Ramdump ELF Helpers */
#define SIZEOF_ELF_STRUCT(__xhdr)					\
static inline size_t sizeof_elf_##__xhdr(unsigned char class)		\
{									\
	if (class == ELFCLASS32)					\
		return sizeof(struct elf32_##__xhdr);			\
	else								\
		return sizeof(struct elf64_##__xhdr);			\
}

SIZEOF_ELF_STRUCT(phdr)
SIZEOF_ELF_STRUCT(hdr)

#define set_xhdr_property(__xhdr, arg, class, member, value)		\
do {									\
	if (class == ELFCLASS32)					\
		((struct elf32_##__xhdr *)arg)->member = value;		\
	else								\
		((struct elf64_##__xhdr *)arg)->member = value;		\
} while (0)

#define set_ehdr_property(arg, class, member, value) \
	set_xhdr_property(hdr, arg, class, member, value)
#define set_phdr_property(arg, class, member, value) \
	set_xhdr_property(phdr, arg, class, member, value)

static struct mhi_channel_config mhitest_mhi_channels[] = {
	{
		.num = 0,
		.name = "LOOPBACK",
		.num_elements = 32,
		.event_ring = 0,
		.dir = DMA_TO_DEVICE,
		.ee_mask = 0x4,
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = false,
	},
	{
		.num = 1,
		.name = "LOOPBACK",
		.num_elements = 32,
		.event_ring = 0,
		.dir = DMA_FROM_DEVICE,
		.ee_mask = 0x4,
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = false,
	},
	{
		.num = 4,
		.name = "DIAG",
		.num_elements = 32,
		.event_ring = 1,
		.dir = DMA_TO_DEVICE,
		.ee_mask = 0x4,
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = false,
	},
	{
		.num = 5,
		.name = "DIAG",
		.num_elements = 32,
		.event_ring = 1,
		.dir = DMA_FROM_DEVICE,
		.ee_mask = 0x4,
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = false,
	},
	{
		.num = 20,
		.name = "IPCR",
		.num_elements = 64,
		.event_ring = 1,
		.dir = DMA_TO_DEVICE,
		.ee_mask = 0x4,
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = false,
	},
	{
		.num = 21,
		.name = "IPCR",
		.num_elements = 64,
		.event_ring = 1,
		.dir = DMA_FROM_DEVICE,
		.ee_mask = 0x4,
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = true,
	},
};

static struct mhi_event_config mhitest_mhi_events[] = {
	{
		.num_elements = 32,
		.irq_moderation_ms = 0,
		.irq = 1,
		.mode = MHI_DB_BRST_DISABLE,
		.data_type = MHI_ER_CTRL,
		.hardware_event = false,
		.client_managed = false,
		.offload_channel = false,
	},
	{
		.num_elements = 256,
		.irq_moderation_ms = 1,
		.irq = 2,
		.mode = MHI_DB_BRST_DISABLE,
		.priority = 1,
		.hardware_event = false,
		.client_managed = false,
		.offload_channel = false,
	},
};

static struct mhi_controller_config mhitest_mhi_config = {
	.max_channels = 128,
	.timeout_ms = 2000,
	.use_bounce_buf = false,
	.buf_len = 0,
	.num_channels = ARRAY_SIZE(mhitest_mhi_channels),
	.ch_cfg = mhitest_mhi_channels,
	.num_events = ARRAY_SIZE(mhitest_mhi_events),
	.event_cfg = mhitest_mhi_events,
};

static struct mhitest_msi_config msi_config = {
	.total_vectors = 16,
	.total_users = 1,
	.users = (struct mhitest_msi_user[]) {
		{ .name = "MHI-TEST", .num_vectors = 16, .base_vector = 0 },
	},
};

struct ramdump_entry {
	__le64 base_address;
	__le64 actual_phys_address;
	__le64 size;
	char description[20];
	char file_name[20];
};

struct ramdump_header {
	__le32 version;
	__le32 header_size;
	struct ramdump_entry ramdump_table[MAX_RAMDUMP_TABLE_SIZE];
};

irqreturn_t mhitest_msi_handlr(int irq_number, void *dev)
{
	pr_info("mhitest_msi_handlr irq_number==%d\n", irq_number);
	return IRQ_HANDLED;
}

void mhitest_get_crash_reason(struct mhi_controller *mhi_cntrl)
{
	struct ramdump_header *ramdump_header;
	struct ramdump_entry *ramdump_table;
	char *msg = ERR_PTR(-EPROBE_DEFER);
	struct image_info *rddm_image;
	u64 coredump_offset = 0;
	struct mhi_buf *mhi_buf;
	struct pci_dev *pdev;
	struct device *dev;
	int i;

	rddm_image = mhi_cntrl->rddm_image;
	mhi_buf = rddm_image->mhi_buf;

	dev = &mhi_cntrl->mhi_dev->dev;
	pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	dev_err(dev, "CRASHED - [DID:DOMAIN:BUS:SLOT] - %x:%04u:%02u:%02u\n",
		pdev->device, pdev->bus->domain_nr, pdev->bus->number,
		PCI_SLOT(pdev->devfn));

	/* Get RDDM header size */
	ramdump_header = (struct ramdump_header *)mhi_buf[0].buf;
	ramdump_table = ramdump_header->ramdump_table;
	coredump_offset += le32_to_cpu(ramdump_header->header_size);

	/* Traverse ramdump table to get coredump offset */
	i = 0;
	while (i < MAX_RAMDUMP_TABLE_SIZE) {
		if (!strncmp(ramdump_table->description, COREDUMP_DESC,
			     sizeof(COREDUMP_DESC)) ||
			!strncmp(ramdump_table->description, Q6_SFR_DESC,
			     sizeof(Q6_SFR_DESC))) {
			break;
		}
		coredump_offset += cpu_to_le64(ramdump_table->size);
		ramdump_table++;
		i++;
	}

	if (i == MAX_RAMDUMP_TABLE_SIZE) {
		dev_err(dev, "Cannot find '%s' entry in ramdump\n",
			COREDUMP_DESC);
		return;
	}

	/* Locate coredump data from the ramdump segments */
	for (i = 0; i < rddm_image->entries; i++) {
		if (coredump_offset < mhi_buf[i].len) {
			msg = mhi_buf[i].buf + coredump_offset;
			break;
		}

		coredump_offset -= mhi_buf[i].len;
	}

	if (!IS_ERR(msg) && msg && msg[0])
		dev_err(dev, "Fatal error received from wcss software!\n%s\n",
			msg);
}

static int mhitest_coredump_build_dump_info(struct mhitest_platform *mplat,
					    struct mhitest_dump_seg *segments,
					    u32 num_seg,
					    struct mhitest_dump_seg **chunks_out,
					    u32 *num_chunks)
{
	struct mhitest_dump_entry tmp[FW_DUMP_TYPE_MAX] = {0};
	int i, type, num_dump_types = 0;
	struct mhitest_dump_meta_info *meta_info;
	struct mhitest_dump_entry *entry;
	struct mhitest_dump_seg *chunks;
	void *data;
	size_t data_len;

	for (i = 0; i < num_seg; i++) {
		type = segments[i].type;
		if (type < 0 || type >= FW_DUMP_TYPE_MAX)
			continue;

		if (tmp[type].entry_num == 0)
			tmp[type].entry_start = i + 1;
		tmp[type].type = type;
		tmp[type].entry_num++;
	}

	/* to find the number of dump types */
	for (i = 0; i < FW_DUMP_TYPE_MAX; i++)
		if (tmp[i].entry_num)
			num_dump_types++;

	data_len = ALIGN(sizeof(struct mhitest_dump_meta_info) +
			num_dump_types * sizeof(struct mhitest_dump_entry), 4);
	data = kzalloc(data_len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	meta_info = data;
	meta_info->magic = MHITEST_RAMDUMP_MAGIC;
	meta_info->version = MHITEST_RAMDUMP_VERSION_V2;
	meta_info->chipset = mplat->device_id;
	meta_info->total_entries = num_dump_types;

	entry = meta_info->entry;
	for (i = 0; i < FW_DUMP_TYPE_MAX; i++) {
		if (tmp[i].entry_num) {
			*entry = tmp[i];
			entry++;
		}
	}

	chunks = kcalloc(1 + num_seg, sizeof(*chunks), GFP_KERNEL);
	if (!chunks) {
		kfree(data);
		return -ENOMEM;
	}

	chunks[0].address   = 0;
	chunks[0].v_address = data;
	chunks[0].size      = data_len;
	chunks[0].type      = 0xFFFFFFFF;

	for (i = 0; i < num_seg; i++)
		chunks[1 + i] = segments[i];

	*chunks_out = chunks;
	*num_chunks = 1 + num_seg;

	pr_info("Dumping meta_info: total_entries: %d",
		meta_info->total_entries);
	for (i = 0; i < meta_info->total_entries; i++)
		pr_info("entry %d type %d entry_start %d entry_num %d",
			i, meta_info->entry[i].type,
			meta_info->entry[i].entry_start,
			meta_info->entry[i].entry_num);

	return 0;
}


static int mhitest_coredump_build_elf32(struct mhitest_platform *mplat,
					struct mhitest_dump_seg *segments,
					u32 num_seg,
					struct mhitest_pci_elf_coredump_state **out_state)
{
	struct mhitest_pci_elf_coredump_state *st;
	Elf32_Ehdr *ehdr;
	Elf32_Phdr *phdr;
	void *elf_hdr;
	struct mhitest_dump_seg *chunks;
	u32 num_chunks;
	u16 phnum;
	u32 elf_hdr_sz, cur_off, i, paddr32;
	int ret;

	ret = mhitest_coredump_build_dump_info(mplat, segments, num_seg,
					       &chunks, &num_chunks);
	if (ret)
		return ret;

	phnum = num_chunks;
	elf_hdr_sz = sizeof(Elf32_Ehdr) + phnum * sizeof(Elf32_Phdr);

	elf_hdr = vzalloc(elf_hdr_sz);
	if (!elf_hdr) {
		kfree(chunks[0].v_address);
		kfree(chunks);
		return -ENOMEM;
	}

	/* ELF header */
	ehdr = (Elf32_Ehdr *)elf_hdr;
	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELFCLASS32;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELFOSABI_NONE;
	ehdr->e_type = cpu_to_le16(ET_CORE);
	ehdr->e_machine = cpu_to_le16(EM_NONE);
	ehdr->e_version = cpu_to_le32(EV_CURRENT);
	ehdr->e_entry = cpu_to_le32(0);
	ehdr->e_phoff = cpu_to_le32(sizeof(Elf32_Ehdr));
	ehdr->e_shoff = cpu_to_le32(0);
	ehdr->e_flags = cpu_to_le32(0);
	ehdr->e_ehsize = cpu_to_le16(sizeof(Elf32_Ehdr));
	ehdr->e_phentsize = cpu_to_le16(sizeof(Elf32_Phdr));
	ehdr->e_phnum = cpu_to_le16(phnum);
	ehdr->e_shentsize = cpu_to_le16(0);
	ehdr->e_shnum = cpu_to_le16(0);
	ehdr->e_shstrndx = cpu_to_le16(0);

	/* Program headers */
	phdr = (Elf32_Phdr *)((u8 *)elf_hdr + sizeof(Elf32_Ehdr));
	cur_off = elf_hdr_sz;

	for (i = 0; i < phnum; i++) {
		paddr32 = (i == 0) ? 0 : (u32)chunks[i].address;
		phdr[i].p_type   = cpu_to_le32(PT_LOAD);
		phdr[i].p_offset = cpu_to_le32(cur_off);
		phdr[i].p_vaddr  = cpu_to_le32(paddr32);
		phdr[i].p_paddr  = cpu_to_le32(paddr32);
		phdr[i].p_filesz = cpu_to_le32(chunks[i].size);
		phdr[i].p_memsz  = cpu_to_le32(chunks[i].size);
		phdr[i].p_flags  = cpu_to_le32(PF_R | PF_W | PF_X);
		phdr[i].p_align  = cpu_to_le32(0);
		cur_off += chunks[i].size;
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		vfree(elf_hdr);
		kfree(chunks[0].v_address);
		kfree(chunks);
		return -ENOMEM;
	}

	st->elf_hdr    = elf_hdr;
	st->elf_hdr_sz = elf_hdr_sz;
	st->chunks     = chunks;
	st->num_chunks = phnum;

	init_completion(&st->dump_done);
	*out_state = st;
	return 0;
}

static ssize_t mhitest_coredump_pci_read(char *buffer, loff_t offset,
					 size_t count, void *data,
					 size_t header_size)
{
	struct mhitest_pci_elf_coredump_state *st = data;
	size_t bytes_left = count;
	struct mhitest_dump_seg *seg = NULL;
	size_t copied = 0;
	int i;
	unsigned long data_left;
	size_t seg_copy_sz, copy_size, hdr_copy;
	void *addr;
	loff_t cur;

	/* Copy ELF header first */
	if (offset < header_size) {
		hdr_copy = min_t(size_t, bytes_left, header_size - offset);
		memcpy(buffer, (u8 *)st->elf_hdr + offset, hdr_copy);
		return hdr_copy;
	}

	offset -= header_size;

	while (bytes_left) {
		cur = 0;
		seg = NULL;

		for (i = 0; i < st->num_chunks; i++) {
			if (offset < cur + st->chunks[i].size) {
				seg = &st->chunks[i];
				break;
			}
			cur += st->chunks[i].size;
		}
		if (!seg)
			break;

		if (offset < cur) {
			pr_err("Invalid offset calculation: offset=%lld < cur=%lld\n",
			       (long long)offset, (long long)cur);
			break;
		}

		data_left = offset - cur;
		if (data_left >= seg->size) {
			pr_err("Invalid offset: data_left=%lu >= seg->size=%lu\n",
			       data_left, seg->size);
			break;
		}

		seg_copy_sz = seg->size - data_left;
		copy_size = min_t(size_t, bytes_left, seg_copy_sz);
		addr = (u8 *)seg->v_address + data_left;
		memcpy(buffer, addr, copy_size);

		offset     += copy_size;
		buffer     += copy_size;
		bytes_left -= copy_size;
		copied     += copy_size;
	}

	return copied;
}

static void mhitest_coredump_pci_free(void *data)
{
	struct mhitest_pci_elf_coredump_state *st = data;

	complete(&st->dump_done);
}


int mhitest_dump_info(struct mhitest_platform *mplat, bool in_panic)
{
	struct mhi_controller *mhi_ctrl = mplat->mhi_ctrl;
	struct image_info *rddm_img, *fw_img;
	struct mhitest_dump_seg *segment, *seg_info;
	int ret = 0, i, num_seg, len, seg_sz, skip_count = 0;
	u16 device_id;
	struct device *dev = &mplat->plat_dev->dev;
	struct mhitest_pci_elf_coredump_state *st = NULL;

	/* Validate device */
	pci_read_config_word(mplat->pci_dev, PCI_DEVICE_ID, &device_id);
	pr_debug("Read config space again, Device_id:0x%x\n", device_id);
	if (device_id != mplat->pci_dev_id->device) {
		pr_debug("Device Id does not match with Probe ID..\n");
		return -EIO;
	}

	/* Download RDDM image */
	ret = mhi_download_rddm_image(mhi_ctrl, in_panic);
	if (ret) {
		pr_debug("Error .. not able to dload rddm img ret:%d\n", ret);
		return ret;
	}

	/* Get crash reason */
	mhitest_get_crash_reason(mhi_ctrl);

	/* Prepare dump data structures */
	rddm_img = mhi_ctrl->rddm_image;
	fw_img = mhi_ctrl->fbc_image;
	num_seg = fw_img->entries + rddm_img->entries;
	len = num_seg * sizeof(*segment);

	/* Allocate segment array for FW and RDDM entries */
	segment = kzalloc(len, GFP_KERNEL);
	if (!segment)
		return -ENOMEM;

	seg_info = segment;

	/* Copy FW image segments */
	for (i = 0; i < fw_img->entries; i++) {
		if (!fw_img->mhi_buf[i].buf) {
			skip_count++;
			continue;
		}
		seg_sz = fw_img->mhi_buf[i].len;
		seg_info->size = PAGE_ALIGN(seg_sz);
		seg_info->address = fw_img->mhi_buf[i].dma_addr;
		seg_info->v_address = fw_img->mhi_buf[i].buf;
		seg_info->type = FW_IMAGE;
		seg_info++;
	}

	/* Copy RDDM image segments */
	for (i = 0; i < rddm_img->entries; i++) {
		if (!rddm_img->mhi_buf[i].buf) {
			skip_count++;
			continue;
		}
		seg_sz = rddm_img->mhi_buf[i].len;
		seg_info->size = PAGE_ALIGN(seg_sz);
		seg_info->address = rddm_img->mhi_buf[i].dma_addr;
		seg_info->v_address = rddm_img->mhi_buf[i].buf;
		seg_info->type = FW_RDDM;
		seg_info++;
	}

	num_seg = num_seg - skip_count;
	ret = mhitest_coredump_build_elf32(mplat, segment, num_seg, &st);
	if (ret) {
		pr_err("ELF32 build failed:%d\n", ret);
		kfree(segment);
		return ret;
	}

	dev_coredumpm(dev, THIS_MODULE, st, st->elf_hdr_sz, GFP_KERNEL,
		      mhitest_coredump_pci_read, mhitest_coredump_pci_free);

	ret = wait_for_completion_timeout(&st->dump_done,
					  msecs_to_jiffies(TIMEOUT_SAVE_DUMP_MS));
	if (!ret) {
		pr_err("Coredump: Timed out waiting to save the dump\n");
		ret = -ETIMEDOUT;
	}

	vfree(st->elf_hdr);
	kfree(st->chunks[0].v_address);
	kfree(st->chunks);
	kfree(st);
	kfree(segment);

	return ret;
}

static int mhitest_get_msi_user(struct mhitest_platform *mplat, char *u_name,
		int *num_vectors, u32 *user_base_data, u32 *base_vector)
{
	int idx;
	struct mhitest_msi_config *m_config = mplat->msi_config;

	if (!m_config) {
		pr_err("MSI config is NULL\n");
		return -ENODEV;
	}

	for (idx = 0; idx < m_config->total_users; idx++) {
		if (strcmp(u_name, m_config->users[idx].name) == 0) {
			*num_vectors = m_config->users[idx].num_vectors;
			*user_base_data = m_config->users[idx].base_vector
				+ mplat->msi_ep_base_data;
			*base_vector = m_config->users[idx].base_vector;
			pr_debug("Assign MSI to user:%s,num_vectors:%d,user_base_data:%u, base_vector: %u\n",
				 u_name, *num_vectors, *user_base_data,
								*base_vector);

			return 0;
		}
	}
	return -ENODEV;
}

static int mhitest_get_msi_irq(struct device  *device, unsigned int vector)
{
	int irq_num;
	struct pci_dev *pci_dev = to_pci_dev(device);

	irq_num = pci_irq_vector(pci_dev, vector);
	pr_debug("Got irq_num :%d  for vector : %d\n", irq_num, vector);

	return irq_num;
}

int mhitest_suspend_pci_link(struct mhitest_platform *mplat)
{
	/*
	 * no suspend resume as of now, return 0
	 */
	pr_info("No suspend resume now return 0\n");
	return 0;
}

int mhitest_resume_pci_link(struct mhitest_platform *mplat)
{
	/*
	 * no suspend resume as of now, return 0
	 */
	pr_info("No suspend resume now return 0\n");
	return 0;
}

int  mhitest_power_off_device(struct mhitest_platform *mplat)
{
	/*
	 * add pinctrl code here if needed !
	 */
	pr_info("Powering OFF dummy!\n");
	return 0;
}

int  mhitest_power_on_device(struct mhitest_platform *mplat)
{

	/*
	 * add pinctrl code here if needed !
	 */
	pr_info("Powering ON dummy!\n");
	return 0;
}

int mhitest_pci_get_link_status(struct mhitest_platform *mplat)
{
	u16 link_stat;
	int ret;

	ret = pcie_capability_read_word(mplat->pci_dev, PCI_EXP_LNKSTA,
							&link_stat);
	if (ret) {
		pr_err("PCIe link is not active !!ret:%d\n", ret);
		return ret;
	}
	pr_debug("Get PCI link status register: %u\n", link_stat);

	mplat->def_link_speed = link_stat & PCI_EXP_LNKSTA_CLS;
	mplat->def_link_width =
		(link_stat & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;

	pr_debug("Default PCI link speed is 0x%x, link width is 0x%x\n",
		 mplat->def_link_speed, mplat->def_link_width);

	return ret;
}

int mhitest_pci_get_mhi_msi(struct mhitest_platform *mplat)
{
	int ret, *irq, num_vectors, i;
	u32 user_base_data, base_vector;

	/*
	 * right now we have only one user in mhitest i.e MHI
	 */
	ret = mhitest_get_msi_user(mplat, "MHI-TEST", &num_vectors,
					&user_base_data, &base_vector);
	if (ret) {
		pr_err("Not able to get msi user ret:%d\n", ret);
		return ret;
	}

	pr_info("MSI user:%s has num_vectors:%d and base_vector:%d\n",
		"MHI-TEST", num_vectors, base_vector);

	irq = kcalloc(num_vectors, sizeof(int), GFP_KERNEL);
	if (!irq) {
		pr_err("Error not able to allocate vectors\n");
		return -ENOMEM;
	}
	for (i = 0; i < num_vectors; i++)
		irq[i] = mhitest_get_msi_irq(&mplat->pci_dev->dev,
							base_vector + i);

	mplat->mhi_ctrl->irq = irq;
	mplat->mhi_ctrl->nr_irqs = num_vectors;

	pr_debug("irq:[%p] msi_allocated :%d\n", mplat->mhi_ctrl->irq,
		 mplat->mhi_ctrl->nr_irqs);

	return 0;
}

char *mhitest_get_reson_str(enum mhi_callback reason)
{
	switch (reason) {
	case MHI_CB_IDLE:
		return "IDLE";
	case MHI_CB_EE_RDDM:
		return "RDDM";
	case MHI_CB_EE_MISSION_MODE:
		return "EE_MISSION_MODE";
	case MHI_CB_BW_REQ:
		return "BW_REQ";
	case MHI_CB_SYS_ERROR:
		return "SYS_ERROR";
	case MHI_CB_FATAL_ERROR:
		return "FATAL_ERROR";
	default:
		return "UNKNOWN";
	}
}

char *mhitest_event_to_str(enum mhitest_event_type etype)
{
	switch (etype) {
	case MHITEST_RECOVERY_EVENT:
		return "MHITEST_RECOVERY_EVENT";
	default:
		return "UNKNOWN EVENT";
	}
}

int mhitest_post_event(struct mhitest_platform *mplat,
		       struct mhitest_recovery_data *data,
		       enum mhitest_event_type etype,
		       u32 flags)
{
	struct mhitest_driver_event *event;
	int gfp = GFP_KERNEL;
	unsigned long irq_flags;

	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	event = kzalloc(sizeof(*event), gfp);
	if (!event)
		return -ENOMEM;

	event->type = etype;
	event->data = data;
	init_completion(&event->complete);
	event->ret = -1;
	event->sync = !!(flags);

	spin_lock_irqsave(&mplat->event_lock, irq_flags);
	list_add_tail(&event->list, &mplat->event_list);
	spin_unlock_irqrestore(&mplat->event_lock, irq_flags);

	queue_work(mplat->event_wq, &mplat->event_work);
	if (flags) {
		pr_err("Waiting here to complete (%s) event ...\n",
		       mhitest_event_to_str(etype));
		wait_for_completion(&event->complete);
	}
	pr_info("No waiting/Completed (%s) event ...ret:%d\n",
		mhitest_event_to_str(etype), event->ret);

	return 0;
}

void mhitest_sch_do_recovery(struct mhitest_platform *mplat,
					enum mhitest_recovery_reason reason)
{
	int gfp = GFP_KERNEL;
	struct mhitest_recovery_data *data;

	/* Check if device is running using atomic operation */
	if (!atomic_read(&mplat->running)) {
		pr_info("Device not running, skipping recovery scheduling\n");
		return;
	}

	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	data = kzalloc(sizeof(*data), gfp);
	if (!data)
		return;

	data->reason = reason;

	mhitest_post_event(mplat, data, MHITEST_RECOVERY_EVENT, 0);
}

int __must_check mhitest_read_reg(struct mhi_controller *mhi_cntrl,
				  void __iomem *addr, u32 *out)
{
	u32 tmp = readl(addr);

	/* If the value is invalid, the link is down */
	if (PCI_INVALID_READ(tmp))
		return -EIO;

	*out = tmp;

	return 0;
}

void mhitest_write_reg(struct mhi_controller *mhi_cntrl, void __iomem *addr,
		       u32 val)
{
	writel(val, addr);
}

void mhitest_mhi_notify_status(struct mhi_controller *mhi_cntrl,
						enum mhi_callback cb_reason)
{
	struct mhitest_platform *temp;
	enum mhitest_recovery_reason reason = MHI_DEFAULT;

	temp = dev_get_drvdata(mhi_cntrl->cntrl_dev);

	pr_debug("Enter\n");
	if (cb_reason > MHI_CB_FATAL_ERROR) {
		pr_err("Unsupported reason :%d\n", reason);
		return;
	}
	pr_emerg(":[%s]- %d\n", mhitest_get_reson_str(cb_reason), cb_reason);

	switch (cb_reason) {
	case MHI_CB_IDLE:
	case MHI_CB_SYS_ERROR:
	case MHI_CB_FATAL_ERROR:
		return;
	case MHI_CB_EE_RDDM:
		reason = MHI_RDDM;
		if (temp->soc_reset_requested) {
			pr_info("Ignoring RDDM CB for SOC_RESET_REQUEST\n");
			temp->soc_reset_requested = false;
			complete(&temp->soc_reset_request);
			return;
		}

		/* check duplicate RDDM received from MHI */
		if (mhi_get_exec_env(mhi_cntrl) == mhi_cntrl->ee) {
			pr_info("Skip duplicate %s(%d) received from MHI\n",
				mhitest_get_reson_str(cb_reason), cb_reason);
			return;
		}
		break;
	case MHI_CB_EE_MISSION_MODE:
		del_timer(&temp->boot_debug_timer);
		pr_debug("MHI_CB_EE_MISSION_MODE\n");
		return;
	case MHI_CB_BW_REQ:
		pr_debug("MHI_CB_BW_REQ\n");
		return;
	default:
		pr_err("Unsupported reason --reason:[%s]-(%d)\n",
		       mhitest_get_reson_str(cb_reason), cb_reason);
		return;
	}
	mhitest_sch_do_recovery(temp, reason);
	pr_debug("Exit\n");
}

int mhitest_mhi_pm_runtime_get(struct mhi_controller *mhi_cntrl)
{
	return 0;
}

void mhitest_mhi_pm_runtime_put_noidle(struct mhi_controller *mhi_cntrl)
{
}

int mhitest_pci_register_mhi(struct mhitest_platform *mplat)
{
	struct pci_dev *pci_dev = mplat->pci_dev;
	struct mhi_controller *mhi_ctrl;
	struct device_node *np;
	int ret, i = 0;
	struct resource memory;

	mhi_ctrl = mhi_alloc_controller();
	if (!mhi_ctrl) {
		pr_err("Error: not able to allocate mhi_ctrl\n");
		return -ENOMEM;
	}
	pr_info("MHI CTRL :%p\n", mhi_ctrl);

	mplat->mhi_ctrl = mhi_ctrl;
	dev_set_drvdata(&pci_dev->dev, mplat);
	mhi_ctrl->cntrl_dev = &pci_dev->dev;

	mhi_ctrl->fw_image = mplat->fw_name;

	mhi_ctrl->regs = mplat->bar;
	mhi_ctrl->reg_len = pci_resource_len(pci_dev, PCI_BAR_NUM);
	pr_emerg("BAR start at :%p Size is %zx\n",
		 &pci_resource_start(pci_dev, PCI_BAR_NUM),
		 mhi_ctrl->reg_len);

	ret  =  mhitest_pci_get_mhi_msi(mplat);
	if (ret) {
		pr_err("PCI get MHI MSI Failed ret:%d\n", ret);
		goto out;
	}

	np = of_find_node_by_type(NULL, "memory");
	if (!np) {
		pr_err("memory node not found !!\n");
		ret = -ENOMEM;
		goto out;
	}

	while (of_address_to_resource(np, i, &memory) == 0) {
		if (!i)
			mhi_ctrl->iova_start = memory.start;
		mhi_ctrl->iova_stop = memory.end;
		i++;
	}

	if (!mhi_ctrl->iova_start || !mhi_ctrl->iova_stop) {
		pr_err("Unable to get resource: memory");
		ret = -ENOMEM;
		goto out;
	}

	pr_debug("iova_start:%llx iova_stop:%llx\n", (u64)mhi_ctrl->iova_start,
		 (u64)mhi_ctrl->iova_stop);

	mhi_ctrl->status_cb = mhitest_mhi_notify_status;
	mhi_ctrl->runtime_get = mhitest_mhi_pm_runtime_get;
	mhi_ctrl->runtime_put = mhitest_mhi_pm_runtime_put_noidle;
	mhi_ctrl->read_reg = mhitest_read_reg;
	mhi_ctrl->write_reg = mhitest_write_reg;

	mhi_ctrl->rddm_size = mplat->mhitest_rdinfo.ramdump_size;
	mhi_ctrl->sbl_size = SZ_512K;
	mhi_ctrl->seg_len = SZ_512K;
	mhi_ctrl->fbc_download = true;

	if (mplat->device_id == QCN96XX_DEVICE_ID ||
	    mplat->device_id == QCN95XX_DEVICE_ID ||
	    mplat->device_id == QCC20XX_DEVICE_ID)
		mhi_ctrl->standard_elf_image = true;

	ret = mhi_register_controller(mhi_ctrl, &mhitest_mhi_config);
	if (ret) {
		pr_err("Failed to register mhi controller ret:%d\n", ret);
		goto out;
	}

	pr_debug("GOOD!\n");
	return  0;

out:
	mhi_free_controller(mhi_ctrl);
	return ret;
}

int mhitest_pci_en_msi(struct mhitest_platform *temp)
{
	struct pci_dev *pci_dev = temp->pci_dev;
	int num_vectors, ret = 0;
	struct msi_desc *msi_desc;

	temp->msi_config = &msi_config;
	if (!temp->msi_config) {
		pr_err("MSI config is NULL\n");
		return -EINVAL;
	}

	num_vectors = pci_alloc_irq_vectors(pci_dev,
					   temp->msi_config->total_vectors,
					   temp->msi_config->total_vectors,
					   PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	if (num_vectors != temp->msi_config->total_vectors) {
		pr_err("No Enough MSI vectors req:%d and allocated:%d\n",
		       temp->msi_config->total_vectors, num_vectors);
		if (num_vectors >= 0)
			ret = -EINVAL;
		temp->msi_config = NULL;
		goto out;
	}

	msi_desc = irq_get_msi_desc(pci_dev->irq);
	if (!msi_desc) {
		pr_err("MSI desc is NULL\n");
		goto free_irq_vectors;
	}

	return 0;

free_irq_vectors:
	pci_free_irq_vectors(pci_dev);
out:
	return ret;
}

int mhitest_pci_enable_bus(struct mhitest_platform *temp)
{
	struct pci_dev *pci_dev = temp->pci_dev;
	u16 device_id;
	int ret;
	u32 pci_dma_mask = PCI_DMA_MASK_64_BIT;

	pr_debug("Going for PCI Enable bus\n");
	pci_read_config_word(pci_dev, PCI_DEVICE_ID, &device_id);
	pr_emerg("Read config space, Device_id:0x%x\n", device_id);

	if (device_id != temp->pci_dev_id->device) {
		pr_err("Device Id does not match with Probe ID..\n");
		return -EIO;
	}

	ret = pci_assign_resource(pci_dev, PCI_BAR_NUM);
	if (ret) {
		pr_err("Failed to assign PCI resource  Error:%d\n", ret);
		goto out;
	}

	ret = pci_enable_device(pci_dev);
	if (ret) {
		pr_err("Failed to Enable PCI device  Error:%d\n", ret);
		goto out;
	}

	ret = pci_request_region(pci_dev, PCI_BAR_NUM, "mhitest_region");
	if (ret) {
		pr_err("Failed to req. region Error:%d\n", ret);
		goto out2;
	}

	ret = dma_set_mask_and_coherent(&pci_dev->dev, DMA_BIT_MASK(pci_dma_mask));
	if (ret) {
		pr_err("Failed to set dma mask:(%d) ret:%d\n",
		       pci_dma_mask, ret);
		goto out3;
	}

	pci_set_master(pci_dev);

	temp->bar = pci_iomap(pci_dev, PCI_BAR_NUM, 0);
	if (!temp->bar) {
		pr_err("Failed to do PCI IO map ..\n");
		ret = -EIO;
		goto out4;
	}

	pci_save_state(pci_dev);
	temp->pci_dev_default_state = pci_store_saved_state(pci_dev);

	return 0;

out4:
	pci_clear_master(pci_dev);
out3:
	pci_release_region(pci_dev, PCI_BAR_NUM);
out2:
	pci_disable_device(pci_dev);
out:
	return ret;
}

static int mhitest_get_bar_remap_ctrl_offset(struct mhitest_platform *mplat,
					     u32 *reg)
{
	switch (mplat->device_id) {
	case QCN90XX_DEVICE_ID:
		fallthrough;
	case QCN92XX_DEVICE_ID:
		*reg = QCN9224_PCIE_REMAP_BAR_CTRL_OFFSET;
		break;

	case QCC20XX_DEVICE_ID:
		fallthrough;
	case QCN95XX_DEVICE_ID:
		fallthrough;
	case QCN96XX_DEVICE_ID:
		*reg = QCN9625_PCIE_REMAP_BAR_CTRL_OFFSET;
		break;

	default:
		pr_err("Unknown device type 0x%lx\n", mplat->device_id);
		return -ENODEV;
	}

	return 0;
}

static int mhitest_pci_select_window(struct mhitest_platform *mplat, u32 addr)
{
	u32 window = 0, prev_window = 0, curr_window = 0, prev_cleared_window = 0;
	volatile u32 write_val, read_val = 0;
	u32 bar_remap_ctrl_offset = 0;
	int retry = 0;
	void __iomem *bar = NULL;

	if (!mplat || !mplat->bar) {
		pr_err("mplat is NULL or bar not assigned\n");
		return -ENODEV;
	}

	switch (mplat->device_id) {
	case QCC20XX_DEVICE_ID:
	case QCN92XX_DEVICE_ID:
		window = (addr >> WINDOW_SHIFT) & QCN9224_WINDOW_VALUE_MASK;
		break;
	case QCN95XX_DEVICE_ID:
	case QCN96XX_DEVICE_ID:
		window = (addr >> WINDOW_SHIFT) & QCN9625_WINDOW_VALUE_MASK;
		break;

	default:
		pr_err("Unknown device type 0x%lx\n", mplat->device_id);
		return -ENODEV;
	}

	bar = mplat->bar;

	if (mhitest_get_bar_remap_ctrl_offset(mplat, &bar_remap_ctrl_offset)) {
		pr_err("Failed to get bar remap ctrl offset\n");
		return -ENODEV;
	}

	prev_window = readl_relaxed(bar + bar_remap_ctrl_offset);

	/* Clear out last 6 or 7 bits of window register */
	switch (mplat->device_id) {
	case QCC20XX_DEVICE_ID:
	case QCN92XX_DEVICE_ID:
		prev_cleared_window = prev_window & ~(QCN9224_WINDOW_VALUE_MASK);
		break;
	case QCN95XX_DEVICE_ID:
	case QCN96XX_DEVICE_ID:
		prev_cleared_window = prev_window & ~(QCN9625_WINDOW_VALUE_MASK);
		break;

	default:
		pr_err("Unknown device type 0x%lx\n", mplat->device_id);
		return -ENODEV;
	}

	/* Write the new last 6 bits of window register. Only window 1 values
	 * are changed. Window 2 and 3 are unaffected.
	 */
	curr_window = prev_cleared_window | window;

	/* Skip writing into window register if the read value
	 * is same as calculated value.
	 */
	if (curr_window == prev_window)
		return 0;

	write_val = WINDOW_ENABLE_BIT | curr_window;
	writel_relaxed(write_val, bar + bar_remap_ctrl_offset);

	read_val = readl_relaxed(bar + bar_remap_ctrl_offset);

	/* If value written is not yet reflected, wait till it is reflected */
	while ((read_val != write_val) && (retry < 100)) {
		mdelay(1);
		read_val = readl_relaxed(bar + bar_remap_ctrl_offset);
		retry++;
	}

	if (retry >= 100 && read_val != write_val)
		pr_err("retry count: %d", retry);

	return 0;
}

static int mhitest_get_mhi_region_len(struct mhitest_platform *mplat,
				      u32 *reg_start, u32 *reg_end)
{
	switch (mplat->device_id) {
	case QCN92XX_DEVICE_ID:
		fallthrough;
	case QCN95XX_DEVICE_ID:
		fallthrough;
	case QCN96XX_DEVICE_ID:
		fallthrough;
	case QCC20XX_DEVICE_ID:
		*reg_start = QCN9224_PCI_MHIREGLEN_REG;
		*reg_end = QCN9224_PCI_MHI_REGION_END;
		break;
	default:
		pr_err("Unknown device type 0x%lx\n", mplat->device_id);
		return -ENODEV;
	}

	return 0;
}

int mhitest_pci_reg_read(struct mhitest_platform *mplat, u32 addr, u32 *val)
{
	int ret = 0;
	u32 mhi_region_start_reg = 0;
	u32 mhi_region_end_reg = 0;
	unsigned long flags;
	void __iomem *bar = NULL;

	if (!mplat || !mplat->bar) {
		pr_err("mplat is NULL or bar not assigned\n");
		return -ENODEV;
	}

	bar = mplat->bar;

	if (addr < MAX_UNWINDOWED_ADDRESS) {
		*val = readl_relaxed(bar + addr);
		return 0;
	}

	ret = mhitest_get_mhi_region_len(mplat, &mhi_region_start_reg,
					 &mhi_region_end_reg);
	if (ret) {
		pr_err("MHI start and end region not assigned.\n");
		return ret;
	}

	spin_lock_irqsave(&pci_reg_window_lock, flags);
	ret = mhitest_pci_select_window(mplat, addr);
	if (ret) {
		pr_err("Failed to select window %d\n", ret);
		goto out;
	}

	if ((addr >= PCIE_LOCAL_REG_BASE && addr <= PCIE_LOCAL_REG_END) ||
	    (addr >= mhi_region_start_reg && addr <= mhi_region_end_reg)) {
		if (addr >= mhi_region_start_reg && addr <= mhi_region_end_reg)
			addr = addr - mhi_region_start_reg;

		*val = readl_relaxed(bar + (addr & WINDOW_RANGE_MASK));
	} else {
		*val = readl_relaxed(bar + WINDOW_START + (addr & WINDOW_RANGE_MASK));
	}

out:
	spin_unlock_irqrestore(&pci_reg_window_lock, flags);

	return ret;
}

int mhitest_pci_reg_write(struct mhitest_platform *mplat, u32 addr, u32 val)
{
	int ret = 0;
	u32 mhi_region_start_reg = 0;
	u32 mhi_region_end_reg = 0;
	unsigned long flags;
	void __iomem *bar = NULL;

	if (!mplat || !mplat->bar) {
		pr_err("mplat is NULL or bar not assigned\n");
		return -ENODEV;
	}

	bar = mplat->bar;

	if (addr < MAX_UNWINDOWED_ADDRESS) {
		writel_relaxed(val, bar + addr);
		return 0;
	}

	ret = mhitest_get_mhi_region_len(mplat, &mhi_region_start_reg,
					 &mhi_region_end_reg);
	if (ret) {
		pr_err("MHI start and end region not assigned.\n");
		return ret;
	}

	spin_lock_irqsave(&pci_reg_window_lock, flags);

	ret = mhitest_pci_select_window(mplat, addr);
	if (ret) {
		pr_err("Failed to select window %d\n", ret);
		goto out;
	}

	if ((addr >= PCIE_LOCAL_REG_BASE && addr <= PCIE_LOCAL_REG_END) ||
	    (addr >= mhi_region_start_reg && addr <= mhi_region_end_reg)) {
		if (addr >= mhi_region_start_reg && addr <= mhi_region_end_reg)
			addr = addr - mhi_region_start_reg;

		writel_relaxed(val, bar + (addr & WINDOW_RANGE_MASK));
	} else {
		writel_relaxed(val, bar + WINDOW_START + (addr & WINDOW_RANGE_MASK));
	}

out:
	spin_unlock_irqrestore(&pci_reg_window_lock, flags);

	return ret;
}

void mhitest_q6_bcr_reset(struct mhitest_platform *mplat)
{
	u32 device_ee = MHI_EE_MAX;
	u32 count = 10;

	pr_info("Issuing Q6 BCR reset Reset\n");

	mhitest_pci_reg_write(mplat, QCN9625_Q6_BCR_RESET, 1);

	pr_info("Q6 bcr reset issued\n");

	while (count) {
		device_ee = readl_relaxed(mplat->bar + QCN9625_BHI_EXECENV);
		if (device_ee == MHI_EE_PBL) {
			pr_info("Target switched to PBL, reset success, count: %d\n", count);
			return;
		}
		mdelay(soc_reset_delay_ms);
		count--;
	}

	if (device_ee != MHI_EE_PBL)
		pr_info("Failed to switch to PBL after BCR reset\n");
}

void mhitest_global_soc_reset(struct mhitest_platform *mplat)
{
	unsigned long flags;
	u32 val;

	pr_info("Issuing SOC Global Reset\n");

	/*
	 * Enable legacy INTx only for QCN92XX: PBL on this device asserts
	 * INTA during re-initialization after Global SOC reset, which must be
	 * consumed by the Global SOC reset ready INTx handler.
	 */
	if (mplat->device_id == QCN92XX_DEVICE_ID && pci_aer_available()) {
		/* Enable the Legacy MHI INTERRUPT */
		spin_lock_irqsave(&global_soc_reset_lock, flags);
		writel_relaxed(QCN9224_PCIE_EN_LEGACY_MHI,
			       mplat->bar + QCN9224_PCIE_LOCAL_REG_LEGACY_INTX_EN_BNK4);
	}

	val = readl_relaxed(mplat->bar + PCIE_SOC_GLOBAL_RESET_ADDRESS);

	val |= PCIE_SOC_GLOBAL_RESET_V;

	writel_relaxed(val, mplat->bar + PCIE_SOC_GLOBAL_RESET_ADDRESS);

	pr_info("SOC Global reset issued, wait for %d ms\n", soc_reset_delay_ms);

	mdelay(soc_reset_delay_ms);

	/* Need to toggle V bit back otherwise stuck in reset status */
	val &= ~PCIE_SOC_GLOBAL_RESET_V;

	writel_relaxed(val, mplat->bar + PCIE_SOC_GLOBAL_RESET_ADDRESS);

	mdelay(soc_reset_delay_ms);

	val = readl_relaxed(mplat->bar + PCIE_SOC_GLOBAL_RESET_ADDRESS);
	if (val == 0xffffffff)
		pr_err("link down error during global reset\n");

	if (mplat->device_id == QCN92XX_DEVICE_ID && pci_aer_available()) {
		spin_unlock_irqrestore(&global_soc_reset_lock, flags);
		pr_debug("SOC Global reset complete, endpoint ready for register access\n");
	}
}

void mhitest_reset_mhi_state(struct mhitest_platform *mplat)
{
	u32 val = 0;

	val = readl_relaxed(mplat->bar + MHISTATUS);

	pr_info("Setting MHI State to reset, current state: 0x%x", val);
	writel_relaxed(MHICTRL_RESET_MASK, mplat->bar + MHICTRL);
}

void mhitest_pci_disable_bus(struct mhitest_platform *mplat)
{
	struct pci_dev *pci_dev = mplat->pci_dev;
	u32 in_reset = -1, temp = -1, retries = 3;

	del_timer(&mplat->boot_debug_timer);

	mhitest_global_soc_reset(mplat);

	msleep(1000);

	mhitest_reset_mhi_state(mplat);

        while (retries--) {
                temp = readl_relaxed(mplat->mhi_ctrl->regs  + 0x38);
                in_reset = (temp & 0x2) >> 0x1;
		if (in_reset) {
			pr_info("Number of retry left:%d- trying again\n",
				retries);
                        udelay(10);
                        continue;
                }
                break;
        }

	if (in_reset) {
		pr_err("Device failed to exit RESET state\n");
		return;
	}
	pr_emerg("MHI Reset good!\n");

	if (mplat->bar) {
		pci_iounmap(pci_dev, mplat->bar);
		mplat->bar = NULL;
	}

	pci_clear_master(pci_dev);
	pci_release_region(pci_dev, PCI_BAR_NUM);
	if (pci_is_enabled(pci_dev))
		pci_disable_device(pci_dev);
}

int mhitest_unregister_ramdump(struct mhitest_platform *mplat)
{
	struct mhitest_ramdump_info *mhitest_rdinfo = &mplat->mhitest_rdinfo;

	mhitest_rdinfo->ramdump_dev = NULL;
	kfree(mhitest_rdinfo->dump_data_vaddr);
	mhitest_rdinfo->dump_data_vaddr = NULL;
	mhitest_rdinfo->dump_data_valid = false;

	return 0;
}

static u32 mhitest_get_dump_desc_size(struct mhitest_platform *mplat)
{
	u32 descriptor_size = 0;
	u32 segment_len = SZ_4K;
	u32 wlan_sram_size = mplat->mhitest_rdinfo.ramdump_size;

	descriptor_size = (((wlan_sram_size / segment_len) +
			    MHITEST_DUMP_DESC_TOLERANCE) *
			    sizeof(struct mhitest_dump_seg));

	return descriptor_size;
}

int mhitest_register_ramdump(struct mhitest_platform *mplat)
{
	struct mhitest_ramdump_info *mhitest_rdinfo;
	struct mhitest_dump_data *dump_data;
	struct device *dev = &mplat->plat_dev->dev;
	u32 ramdump_size = 0;
	int ret;

	mhitest_rdinfo = &mplat->mhitest_rdinfo;
	dump_data = &mhitest_rdinfo->dump_data;
	if (!dev->of_node) {
		pr_err("of node is null\n");
		return -ENOMEM;
	}
	if (!dev->of_node->name) {
		pr_err("of node->name  is NULL\n");
		return -ENOMEM;
	}

	ret = of_property_read_u32(dev->of_node, "qcom,wlan-ramdump-dynamic",
					 &ramdump_size);
	if (ret == 0)
		mhitest_rdinfo->ramdump_size = ramdump_size;

	mhitest_rdinfo->dump_data_vaddr = kzalloc(
			mhitest_get_dump_desc_size(mplat), GFP_KERNEL);
	if (!mhitest_rdinfo->dump_data_vaddr)
		return -ENOMEM;

	dump_data->paddr = virt_to_phys(mhitest_rdinfo->dump_data_vaddr);

	dump_data->version = 0x22;
	dump_data->magic = 0x42445953;
	dump_data->seg_version = 0x2;
	strlcpy(dump_data->name, "mhitest_mod",
		sizeof(dump_data->name));
	mhitest_rdinfo->ramdump_dev = dev;

	pr_info("Ramdump registered ramdump_size:0x%x\n", ramdump_size);

	return 0;
}

static void mhitest_boot_debug_timeout_hdlr(struct timer_list *timer)
{
	struct mhitest_platform *mplat = from_timer(mplat, timer,
						    boot_debug_timer);

	if (!pblsbl_debug || !mplat || !mplat->mhi_ctrl || atomic_read(&mplat->running) ||
	    MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee) ||
	    mhi_scan_rddm_cookie(mplat->mhi_ctrl, DEVICE_RDDM_COOKIE))
		return;

	pr_debug("Dump MHI/PBL/SBL debug data every %ds during MHI power on\n",
		    BOOT_DEBUG_TIMEOUT_MS / 1000);

	mhi_debug_reg_dump(mplat->mhi_ctrl);
	mhitest_pci_dump_bl_sram_mem(mplat);

	mod_timer(&mplat->boot_debug_timer,
		  jiffies + msecs_to_jiffies(BOOT_DEBUG_TIMEOUT_MS));
}

int mhitest_prepare_pci_mhi_msi(struct mhitest_platform *temp)
{
	int ret;

	pr_debug("Enter\n");
	if (!temp->pci_dev) {
		pr_err("pci_dev is NULL\n");
		return -EINVAL;
	}

	ret = mhitest_register_ramdump(temp);
	if (ret) {
		pr_err("Error not able to reg ramdump. ret :%d\n", ret);
		goto unreg_rdump;
	}

	/*
	 * 1. pci enable bus
	 */
	ret = mhitest_pci_enable_bus(temp);
	if (ret) {
		pr_err("Error mhitest_pci_enable. ret :%d\n", ret);
		goto out;
	}

	/*
	 * go with some condition for specific device for msi en
	 * 2. pci enable msi
	 */
	ret = mhitest_pci_en_msi(temp);
	if (ret) {
		pr_err("Error mhitest_pci_enable_msi. ret :%d\n", ret);
		goto disable_bus;
	}

	/*
	 * 3. pci register mhi -of_controller
	 */
	ret = mhitest_pci_register_mhi(temp);
	if (ret) {
		pr_err("Error pci register mhi. ret :%d\n", ret);
		goto disable_bus;
	}

	ret = mhitest_pci_get_link_status(temp);
	if (ret) {
		pr_err("Error not able to get pci link status:%d\n", ret);
		goto out;
	}

	ret = mhitest_suspend_pci_link(temp);
	if (ret) {
		pr_err("Error not able to suspend pci:%d\n", ret);
		goto out;
	}

	timer_setup(&temp->boot_debug_timer,
		    mhitest_boot_debug_timeout_hdlr, 0);

	mhitest_power_off_device(temp);
	pr_debug("Exit\n");

	return 0;

disable_bus:
	mhitest_pci_disable_bus(temp);
unreg_rdump:
	mhitest_unregister_ramdump(temp);
out:
	return ret;
}

char *mhitest_get_mhi_state_str(enum MHI_STATE state)
{
	switch (state) {
	case MHI_INIT:
		return "MHI_INIT";
	case MHI_DEINIT:
		return "MHI_DEINIT";
	case MHI_POWER_ON:
		return "MHI_POWER_ON";
	case MHI_POWER_OFF:
		return "MHI_POWER_OFF";
	case MHI_FORCE_POWER_OFF:
		return "MHI_FORCE_POWER_OFF";
	default:
		return "UNKNOWN";
	}
}

int mhitest_pci_set_mhi_state(struct mhitest_platform *mplat,
						enum MHI_STATE state)
{
	int ret = 0;
	int i = 0;

	if (state < 0) {
		pr_err("Invalid MHI state : %d\n", state);
		return -EINVAL;
	}

	pr_emerg("Set MHI_STATE- [%s]-(%d)\n",
		 mhitest_get_mhi_state_str(state), state);

	switch (state) {
	case MHI_INIT:
		ret = mhi_prepare_for_power_up(mplat->mhi_ctrl);

		/* Registering dummy interrupt handler for vectors
		 * 3 to 16 to demonstrate the usage of multiple
		 * GIC-MSI interrupts
		 */
		if (!ret && mplat->msi_config->total_vectors > 3) {
			for (i = 3; i < mplat->msi_config->total_vectors; i++) {
				ret = request_irq(mplat->mhi_ctrl->irq[i],
						  mhitest_msi_handlr,
						  IRQF_SHARED,
						  "mhi_rem_vec",
						  mplat->mhi_ctrl);
				if (ret) {
					pr_err("Error requesting irq:%d for vector:%d----error_code-%d\n",
					       mplat->mhi_ctrl->irq[i], i, ret);
				}
			}

			/* Updating ret to 0.
			 * Since vectors 3 t0 16 are unused any failure
			 * in registering interrupt handler should not
			 * affect the flow of FBC.
			 */
			ret = 0;
		}
		break;
	case MHI_POWER_ON:
		ret = mhi_sync_power_up(mplat->mhi_ctrl);
		break;
	case MHI_DEINIT:
		mhi_unprepare_after_power_down(mplat->mhi_ctrl);
		if (mplat->msi_config->total_vectors > 3) {
			for (i = 3; i < mplat->msi_config->total_vectors; i++) {
				free_irq(mplat->mhi_ctrl->irq[i], mplat->mhi_ctrl);
			}
		}
		ret = 0;
		break;
	case MHI_POWER_OFF:
		mhi_power_down(mplat->mhi_ctrl, true);
		ret = 0;
		break;
	case MHI_FORCE_POWER_OFF:
		mhi_power_down(mplat->mhi_ctrl, false);
		ret = 0;
		break;
	default:
		pr_err("I dont know the state:%d!!\n", state);
		ret = -EINVAL;
	}
	return ret;
}

extern int timeout_ms;
int mhitest_pci_start_mhi(struct mhitest_platform *mplat)
{
	int ret;
	u32 val;
	int qrtr_instance_id_reg = PCIE_PCIE_LOCAL_REG_PCIE_LOCAL_RSV0;

	pr_debug("Enter\n");

	if (!mplat->mhi_ctrl) {
		pr_err("mhi_ctrl is NULL .. returning..\n");
		return -EINVAL;
	}

	mplat->mhi_ctrl->timeout_ms = timeout_ms;

	ret = mhitest_pci_set_mhi_state(mplat, MHI_INIT);
	if (ret) {
		pr_err("Error not able to set mhi init. returning..\n");
		goto out1;
	}

	/**
	 * in the single wlan chipset case, plat_priv->qrtr_node_id always is 0,
	 * wlan fw will use the hardcode 7 as the qrtr node id.
	 * in the dual Hastings case, we will read qrtr node id
	 * from device tree and pass to get plat_priv->qrtr_node_id,
	 * which always is not zero. And then store this new value
	 * to pcie register, wlan fw will read out this qrtr node id
	 * from this register and overwrite to the hardcode one
	 * while do initialization for ipc router.
	 * without this change, two Hastings will use the same
	 * qrtr node instance id, which will mess up qmi message
	 * exchange. According to qrtr spec, every node should
	 * have unique qrtr node id
	 */

	switch (mplat->device_id) {
	case QCN90XX_DEVICE_ID:
	case QCN92XX_DEVICE_ID:
		qrtr_instance_id_reg = PCIE_PCIE_LOCAL_REG_PCIE_LOCAL_RSV0;
		break;
	case QCC20XX_DEVICE_ID:
	case QCN95XX_DEVICE_ID:
	case QCN96XX_DEVICE_ID:
		qrtr_instance_id_reg =
				QCN9625_PCIE_PCIE_LOCAL_REG_PCIE_LOCAL_RSV0;
		break;
	default:
		pr_err("Invalid device_id: 0x%lx", mplat->device_id);
		goto out1;
	}

	pr_debug("write 0x%x to qrtr_instance_id_reg\n", mplat->d_instance);
	writel(mplat->d_instance, mplat->bar + qrtr_instance_id_reg);
	msleep(1);
	val = readl(mplat->bar + qrtr_instance_id_reg);

	if (val != mplat->d_instance) {
		pr_err("qrtr node id write to register doesn't match with readout value 0x%x", val);
		goto out1;
	}

	/* Start the timer to dump MHI/PBL/SBL debug data periodically */
	mod_timer(&mplat->boot_debug_timer,
		  jiffies + msecs_to_jiffies(BOOT_DEBUG_TIMEOUT_MS));

	ret = mhitest_pci_set_mhi_state(mplat, MHI_POWER_ON);
	if (ret) {
		pr_err("Error not able to POWER ON\n");
		goto out1;
	}

	pr_debug("Exit\n");
	return ret;

out1:
	if (ret == -ETIMEDOUT) {
		if (!mhi_scan_rddm_cookie(mplat->mhi_ctrl, DEVICE_RDDM_COOKIE)) {
			mhi_debug_reg_dump(mplat->mhi_ctrl);
			mhitest_pci_dump_bl_sram_mem(mplat);
		}
	}

	pr_debug("Exit-Error\n");
	return ret;
}

int mhitest_prepare_start_mhi(struct mhitest_platform *mplat)
{
	int ret;

	if (atomic_read(&mplat->running))
		return 0;

	/*
	 * 1. power on, resume link if needed
	 */
	ret = mhitest_power_on_device(mplat);
	if (ret) {
		pr_err("Error ret:%d\n", ret);
		goto out;
	}
	ret = mhitest_resume_pci_link(mplat);
	if (ret) {
		pr_err("Error ret: %d\n", ret);
		goto out;
	}

	/*
	 * 2. start mhi
	 */
	ret = mhitest_pci_start_mhi(mplat);
	if (ret) {
		pr_err("Error ret: %d\n", ret);
		goto out;
	}
	atomic_set(&mplat->running, 1);

out:
	return ret;
}

extern int domain;

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct mhitest_platform *mplat;
	struct pci_dev *pci_dev = container_of(dev, struct pci_dev, dev);

	mplat = get_mhitest_mplat_by_pcidev(pci_dev);
	if (!mplat)
		return -ENOENT;

	return sysfs_emit(buf, "%s\n", atomic_read(&mplat->running) ? "started" : "stopped");
}

static ssize_t state_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int ret = 0;
	struct mhitest_platform *mplat;
	struct pci_dev *pci_dev = container_of(dev, struct pci_dev, dev);

	mplat = get_mhitest_mplat_by_pcidev(pci_dev);
	if (!mplat)
		return -ENOENT;

	if (sysfs_streq(buf, "start")) {
		ret = mhitest_prepare_start_mhi(mplat);
		if (ret) {
			pr_err("Error preapare start mhi  ret:%d\n", ret);
		}
	} else if (sysfs_streq(buf, "stop")) {
		if (atomic_read(&mplat->running)) {
			mhitest_pci_soc_reset(mplat);
			mhitest_pci_set_mhi_state(mplat, MHI_POWER_OFF);
			mhitest_pci_set_mhi_state(mplat, MHI_DEINIT);
			atomic_set(&mplat->running, 0);
			mhitest_global_soc_reset(mplat);
			msleep(1000);
			mhitest_reset_mhi_state(mplat);
		}
	} else {
		pr_err("Unrecognised option\n");
		ret = -EINVAL;
	}

	return ret ? ret : count;
}

static DEVICE_ATTR_RW(state);

static struct attribute *mhitest_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};

static struct attribute_group mhitest_group = {
	.attrs = mhitest_attrs,
};

static irqreturn_t mhitest_global_soc_reset_ready_intx_handler(int irq, void *dev_id)
{
	struct mhitest_platform *mplat = dev_id;
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&global_soc_reset_lock, flags);

	/* Disable the global interrupt to prevent re-entrancy while handling */
	writel(0x0, mplat->bar + QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_COMN);

	/* Read the interrupt status to identify the source */
	val = readl(mplat->bar + QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_STS4);
	if (!(val & QCN9224_PCIE_EN_LEGACY_MHI)) {
		pr_err("Spurious INTx: status=0x%x, expected MHI bit (0x%x)\n",
		       val, QCN9224_PCIE_EN_LEGACY_MHI);
		/* Re-enable global interrupt before returning */
		writel(0x1, mplat->bar + QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_COMN);
		spin_unlock_irqrestore(&global_soc_reset_lock, flags);
		return IRQ_NONE;
	}

	/* Clear the MHI legacy interrupt status bit */
	writel(QCN9224_PCIE_EN_LEGACY_MHI,
	       mplat->bar + QCN9224_PCIE_LOCAL_REG_LEGACY_INTX_EN_CLR_BNK4);

	/* Read back status register to confirm the clear has taken effect */
	val = readl(mplat->bar + QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_STS4);
	if (val != 0x0)
		pr_err("Interrupt clear failed: status=0x%x after clear, expected 0x0\n", val);

	/* Re-enable the global interrupt now that this INTx has been serviced */
	writel(0x1, mplat->bar + QCN9224_PCIE_LOCAL_REG_DIR_LEGACY_INTX_COMN);

	pr_debug("SOC reset ready INTx handled: PBL signaled endpoint readiness\n");

	spin_unlock_irqrestore(&global_soc_reset_lock, flags);

	return IRQ_HANDLED;
}

static int mhitest_register_global_soc_reset_ready_intx_handler(struct mhitest_platform *mplat)
{
	struct pci_dev *root_port;
	int ret;

	if (!(mplat->device_id == QCN92XX_DEVICE_ID && pci_aer_available()))
		return 0;

	/*
	 * PCIe endpoints typically don't have legacy INTx configured when
	 * MSI/MSI-X is available. The legacy INTx line is routed through the
	 * root port.
	 * This is the same shared IRQ used by AER/PME services.
	 */
	root_port = pcie_find_root_port(mplat->pci_dev);
	if (!root_port) {
		pr_err("Failed to find root port for legacy INTx IRQ\n");
		return -ENODEV;
	}

	/*
	 * Register the SOC reset ready handler on the root port's shared
	 * legacy INTx IRQ. The endpoint PBL asserts INTA after SOC global
	 * reset to notify the host that the SOC has reset and is ready for
	 * MHI reset register write.
	 */
	ret = request_irq(root_port->irq, mhitest_global_soc_reset_ready_intx_handler,
			  IRQF_SHARED, "mhitest_soc_reset_ready", mplat);
	if (ret) {
		pr_err("Failed to register Global SOC reset ready INTx handler on IRQ %d, ret=%d\n",
		       root_port->irq, ret);
		return ret;
	}

	pr_debug("Registered Global SOC reset ready INTx handler on root port IRQ %d\n",
		 root_port->irq);

	return 0;
}

static void mhitest_unregister_global_soc_reset_ready_intx_handler(struct mhitest_platform *mplat)
{
	struct pci_dev *root_port;

	if (!(mplat->device_id == QCN92XX_DEVICE_ID && pci_aer_available()))
		return;

	root_port = pcie_find_root_port(mplat->pci_dev);
	if (!root_port) {
		pr_err("Failed to find root port for legacy INTx IRQ\n");
		return;
	}

	free_irq(root_port->irq, mplat);
	pr_debug("Unregistered Global SOC reset ready INTx handler from IRQ %d\n",
		 root_port->irq);
}

int mhitest_pci_probe(struct pci_dev *pci_dev, const struct pci_device_id *id)
{
	struct mhitest_platform *mplat;
	struct platform_device *plat_dev = get_plat_device();
	int ret;

	if ((domain != -1) && pci_domain_nr(pci_dev->bus) != domain) {
		pr_info("Skipping MHI Device on: %04x:%02x:%02x\n",
			pci_domain_nr(pci_dev->bus),
			pci_dev->bus->number, PCI_SLOT(pci_dev->devfn));
		return 0;
	}

	pr_debug("--->\n");
	mplat = devm_kzalloc(&plat_dev->dev, sizeof(*mplat), GFP_KERNEL);
	if (!mplat) {
		pr_err("Error: not able to allocate memory\n");
		ret = -ENOMEM;
		goto fail_probe;
	}

	if (plat_dev != NULL)
		mplat->plat_dev = plat_dev;
	else
		pr_err("Error: platform dev is broken\n");

	platform_set_drvdata(plat_dev, mplat);

	mplat->pci_dev = pci_dev;
	mplat->device_id = pci_dev->device;
	mplat->pci_dev_id = id;
	mplat->d_instance = u32_encode_bits(pci_domain_nr(pci_dev->bus),
					    DOMAIN_NR_MASK) |
			    u32_encode_bits(pci_dev->bus->number, BUS_NR_MASK);

	pr_info("Vendor ID:0x%x Device ID:0x%x Probed Device ID:0x%x Instance ID:0x%x\n",
		pci_dev->vendor, pci_dev->device, id->device,
		mplat->d_instance);

	ret = mhitest_event_work_init(mplat);
	if (ret)
		goto free_mplat;

	/* Initialize recovery synchronization */
	atomic_set(&mplat->recovery_in_progress, 0);
	init_completion(&mplat->recovery_complete);

	mhitest_store_mplat(mplat);

	ret = mhitest_register_global_soc_reset_ready_intx_handler(mplat);
	if (ret)
		goto remove_mplat;

	if (mplat->device_id == QCN92XX_DEVICE_ID)
		snprintf(mplat->fw_name, sizeof(mplat->fw_name),
			 QCN9224_DEFAULT_FW_FILE_NAME);
	else if (mplat->device_id == QCC20XX_DEVICE_ID)
		snprintf(mplat->fw_name, sizeof(mplat->fw_name),
			 QCC2072_DEFAULT_FW_FILE_NAME);
	else if (mplat->device_id == QCN96XX_DEVICE_ID)
		snprintf(mplat->fw_name, sizeof(mplat->fw_name),
			 QCN9625_DEFAULT_FW_FILE_NAME);
	else if (mplat->device_id == QCN95XX_DEVICE_ID)
		snprintf(mplat->fw_name, sizeof(mplat->fw_name),
			 QCN9589_DEFAULT_FW_FILE_NAME);
	else
		snprintf(mplat->fw_name, sizeof(mplat->fw_name),
			 QCN9000_DEFAULT_FW_FILE_NAME);

	ret = mhitest_prepare_pci_mhi_msi(mplat);
	if (ret) {
		pr_err("Error prep. pci_mhi_msi  ret:%d\n", ret);
		goto unreg_ramdump;
	}

	if (autostart) {
		ret = mhitest_prepare_start_mhi(mplat);
		if (ret) {
			pr_err("Error preapare start mhi  ret:%d\n", ret);
			goto pci_deinit;
		}
	} else {
		pr_emerg("MHI autostart is disabled");
	}

	ret = sysfs_create_group(&pci_dev->dev.kobj, &mhitest_group);
	if (ret) {
		pr_err("Unable to create sysfs group ret:%d\n", ret);
		goto pci_deinit;
	}

	pr_debug("<---done\n");
	return 0;

pci_deinit:
	mhitest_pci_set_mhi_state(mplat, MHI_DEINIT);
unreg_ramdump:
	mhitest_unregister_global_soc_reset_ready_intx_handler(mplat);
	mhitest_pci_remove_all(mplat);
	pci_load_and_free_saved_state(pci_dev, &mplat->pci_dev_default_state);
remove_mplat:
	mhitest_remove_mplat(mplat);
	mhitest_event_work_deinit(mplat);
free_mplat:
	devm_kfree(&mplat->plat_dev->dev, mplat);
fail_probe:
	return ret;
}

void mhitest_pci_soc_reset(struct mhitest_platform *mplat)
{
	if (!atomic_read(&mplat->running))
		return;

	if (mhi_get_exec_env(mplat->mhi_ctrl) == MHI_EE_RDDM) {
		pr_info("MHI SOC_RESET is not required as MHI is already in RDDM state\n");
		return;
	}

	init_completion(&mplat->soc_reset_request);
	mplat->soc_reset_requested = true;
	mhi_soc_reset(mplat->mhi_ctrl);
	if (!wait_for_completion_timeout(&mplat->soc_reset_request,
					 msecs_to_jiffies(soc_reset_delay_ms))) {
		pr_err("SOC reset request failed\n");
		mplat->soc_reset_requested = false;
		reinit_completion(&mplat->soc_reset_request);
	}
	pr_info("SOC_RESET_REQ done");
}

void mhitest_pci_remove(struct pci_dev *pci_dev)
{
	struct mhitest_platform *mplat;

	if ((domain != -1) && pci_domain_nr(pci_dev->bus) != domain) {
		pr_info("Skipping MHI Device on: %04x:%02x:%02x\n",
			pci_domain_nr(pci_dev->bus),
			pci_dev->bus->number, PCI_SLOT(pci_dev->devfn));
		return;
	}
	pr_info("mhitest PCI removing\n");

	sysfs_remove_group(&pci_dev->dev.kobj, &mhitest_group);

	mplat = get_mhitest_mplat_by_pcidev(pci_dev);
	if (mplat) {
		pr_debug("Going for shutdown\n");

		/* CRITICAL: Set running=false FIRST to prevent new callbacks
		 * from queuing new work. Atomic operations have implicit memory
		 * barriers, so no explicit barriers needed.
		 */
		atomic_set(&mplat->running, 0);

		/* Wait for any in-progress recovery to complete before cleanup */
		if (atomic_read(&mplat->recovery_in_progress)) {
			pr_info("Waiting for recovery to complete...\n");
			wait_for_completion_timeout(&mplat->recovery_complete,
						    msecs_to_jiffies(30000)); /* 30s timeout */
		}

		/* Now cancel and flush any pending work. Since running=false,
		 * no new work can be queued by callbacks during or after this.
		 */
		if (mplat->event_wq) {
			pr_info("Cancelling pending recovery work\n");
			cancel_work_sync(&mplat->event_work);
			flush_workqueue(mplat->event_wq);
		}

		/* Safe to cleanup resources - no work can access them anymore */
		mhitest_pci_soc_reset(mplat);
		mhitest_pci_set_mhi_state(mplat, MHI_POWER_OFF);
		mhitest_pci_set_mhi_state(mplat, MHI_DEINIT);

		mhitest_pci_remove_all(mplat);
		mhitest_event_work_deinit(mplat);
		/*
		 * Unregister the Global SOC reset ready INTx handler AFTER Global SOC reset and MHI
		 * deinit, so it can consume any INTx asserted by PBL
		 * during re-initialization triggered by the Global SOC reset.
		 * Only registered for QCN92XX.
		 */
		mhitest_unregister_global_soc_reset_ready_intx_handler(mplat);
		pci_load_and_free_saved_state(pci_dev, &mplat->pci_dev_default_state);
		mhitest_free_mplat(mplat);
	}
}

static const char *pcie_channel_state_to_string(pci_channel_state_t state)
{
	switch (state) {
	case pci_channel_io_normal:
		return "pci_channel_io_normal";
	break;
	case pci_channel_io_frozen:
		return "pci_channel_io_frozen";
	break;
	case pci_channel_io_perm_failure:
		return "pci_channel_io_perm_failure";
	break;
	}

	return "Invalid state";
}

static pci_ers_result_t
mhitest_pci_error_detected(struct pci_dev *pdev, pci_channel_state_t state)
{
	struct mhitest_platform *mplat;
	int ret;

	if (!pci_aer_available())
		return PCI_ERS_RESULT_NONE;

	mplat = get_mhitest_mplat_by_pcidev(pdev);
	if (!mplat) {
		pr_err("Failed to get mhitest platform data\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}

	pr_info("%s pci channel state:%s\n", __func__, pcie_channel_state_to_string(state));
	switch (state) {
	/* Link down error */
	case pci_channel_io_frozen:
		ret = mhitest_pci_get_link_status(mplat);
		if (ret) {
			if (ret == PCIBIOS_DEVICE_NOT_FOUND) {
				ret = pci_load_saved_state(pdev, mplat->pci_dev_default_state);
				if (ret) {
					pr_err("Failed to load the default state, ret:%d\n", ret);
					return PCI_ERS_RESULT_DISCONNECT;
				}
				mplat->pci_dev_saved_state = pci_store_saved_state(pdev);
				return PCI_ERS_RESULT_CAN_RECOVER;
			}
			pr_err("Failed to get pci link status:%d\n", ret);
		}
	break;
	}

	return PCI_ERS_RESULT_NONE;
}

static void mhitest_pci_resume(struct pci_dev *pdev)
{
	struct mhi_controller *mhi_ctrl;
	struct mhitest_platform *mplat;
	enum mhi_ee_type mhi_ee;
	struct device *dev;
	int retry = 0;
	int ret;

	if (!pci_aer_available())
		return;

	mplat = get_mhitest_mplat_by_pcidev(pdev);
	if (!mplat) {
		pr_err("Failed to get mhitest platform data\n");
		return;
	}

	ret = mhitest_pci_get_link_status(mplat);
	if (ret) {
		pr_err("Error not able to get pci link status:%d\n", ret);
		return;
	}

	if (mplat->def_link_speed && mplat->def_link_width)
		pr_info("AER Error recovered, now link is up");

	pr_info("LINK RECOVERED - [%x:%04u:%02u:%02u] speed: GEN%d, width:x%d",
		pdev->device,
		pdev->bus->domain_nr,
		pdev->bus->number,
		PCI_SLOT(pdev->devfn),
		mplat->def_link_speed,
		mplat->def_link_width);

	/* Restore the config space */
	pci_load_and_free_saved_state(pdev, &mplat->pci_dev_saved_state);
	pci_restore_state(pdev);

	mhi_ctrl = mplat->mhi_ctrl;
	dev = &mhi_ctrl->mhi_dev->dev;
	pdev = to_pci_dev(mhi_ctrl->cntrl_dev);

retry:
	/*
	 * After PCIe link resumes, 20 to 400 ms delay is observerved
	 * before device moves to RDDM.
	 */
	msleep(RDDM_LINK_RECOVERY_RETRY_DELAY_MS);
	mhi_ee = mhi_get_exec_env(mhi_ctrl);
	if (mhi_ee == MHI_EE_RDDM) {
		dev_info(dev, "Successfully moved to RDDM state\n");
	} else if (retry++ < RDDM_LINK_RECOVERY_RETRY) {
		goto retry;
	} else {
		dev_err(dev,
			"Failed to move to RDDM state. Current EE state %s\n",
			TO_MHI_EXEC_STR(mhi_ee));
	}
}

static const struct pci_device_id mhitest_pci_id_table[] = {
	{QTI_PCI_VENDOR_ID, QCN90XX_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID},
	{QTI_PCI_VENDOR_ID, QCN92XX_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID},
	{QTI_PCI_VENDOR_ID, QCC20XX_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID},
	{QTI_PCI_VENDOR_ID, QCN96XX_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID},
	{QTI_PCI_VENDOR_ID, QCN95XX_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID},
	{}
};

static const struct pci_error_handlers mhitest_pci_err_handler = {
	.error_detected = mhitest_pci_error_detected,
	.resume = mhitest_pci_resume,
};

struct pci_driver mhitest_pci_driver = {
	.name	  = "mhitest_pci",
	.probe	  = mhitest_pci_probe,
	.remove	  = mhitest_pci_remove,
	.id_table = mhitest_pci_id_table,
	.err_handler = &mhitest_pci_err_handler,
};

int mhitest_pci_register(void)
{
	int ret;

	ret = pci_register_driver(&mhitest_pci_driver);
	if (ret) {
		pr_err("Error ret:%d\n", ret);
		goto out;
	}
out:
	return ret;
}

void mhitest_pci_unregister(void)
{
	pr_debug("\n");
	pci_unregister_driver(&mhitest_pci_driver);
}
