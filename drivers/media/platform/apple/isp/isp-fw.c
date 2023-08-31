// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2023 Eileen Yoon <eyn@gmx.com> */

#include <linux/delay.h>
#include <linux/pm_runtime.h>

#include "isp-cmd.h"
#include "isp-iommu.h"
#include "isp-ipc.h"
#include "isp-regs.h"

#define ISP_FIRMWARE_MDELAY	   1
#define ISP_FIRMWARE_MAX_TRIES	   1000

#define ISP_FIRMWARE_BOOTARGS_SIZE 0x180
#define ISP_FIRMWARE_IPC_SIZE	   0x1c000
#define ISP_FIRMWARE_DATA_SIZE	   0x28000

static inline u32 isp_asc_read32(struct apple_isp *isp, u32 reg)
{
	return readl(isp->asc + reg);
}

static inline void isp_asc_write32(struct apple_isp *isp, u32 reg, u32 val)
{
	writel(val, isp->asc + reg);
}

struct isp_firmware_bootargs {
	u32 pad_0[2];
	u64 ipc_iova;
	u64 unk_size;
	u64 unk_inv;
	u64 extra_iova;
	u64 extra_size;
	u32 unk4;
	u32 pad_40[7];
	u32 ipc_size;
	u32 pad_60[5];
	u32 unk5;
	u32 pad_7c[13];
	u32 pad_b0;
	u32 unk7;
	u32 pad_b8[5];
	u32 unk_iova1;
	u32 pad_c0[47];
	u32 unk9;
} __packed;
static_assert(sizeof(struct isp_firmware_bootargs) ==
	      ISP_FIRMWARE_BOOTARGS_SIZE);

struct isp_chan_desc {
	char name[64];
	u32 type;
	u32 src;
	u32 num;
	u32 pad;
	u64 iova;
	u32 padding[0x2a];
} __packed;
static_assert(sizeof(struct isp_chan_desc) == 0x100);

static const struct isp_chan_ops tm_ops = {
	.handle = ipc_tm_handle,
};

static const struct isp_chan_ops sm_ops = {
	.handle = ipc_sm_handle,
};

static const struct isp_chan_ops bt_ops = {
	.handle = ipc_bt_handle,
};

static irqreturn_t apple_isp_isr(int irq, void *dev)
{
	struct apple_isp *isp = dev;

	isp_core_write32(isp, ISP_CORE_IRQ_ACK,
			 isp_core_read32(isp, ISP_CORE_IRQ_INTERRUPT));

	wake_up_interruptible_all(&isp->wait);

	ipc_chan_handle(isp, isp->chan_sm);
	wake_up_interruptible_all(&isp->wait); /* Some commands depend on sm */

	ipc_chan_handle(isp, isp->chan_tm);

	ipc_chan_handle(isp, isp->chan_bt);
	wake_up_interruptible_all(&isp->wait);

	return IRQ_HANDLED;
}

static void isp_disable_irq(struct apple_isp *isp)
{
	isp_core_write32(isp, ISP_CORE_IRQ_ENABLE, 0x0);
	free_irq(isp->irq, isp);
	isp_core_write32(isp, ISP_CORE_GPIO_1, 0xfeedbabe); /* real funny */
}

static int isp_enable_irq(struct apple_isp *isp)
{
	int err;

	err = request_irq(isp->irq, apple_isp_isr, 0, "apple-isp", isp);
	if (err < 0) {
		isp_err(isp, "failed to request IRQ#%u (%d)\n", isp->irq, err);
		return err;
	}

	isp_dbg(isp, "about to enable interrupts...\n");

	isp_core_write32(isp, ISP_CORE_IRQ_ENABLE, 0xf);

	return 0;
}

static int isp_coproc_ready(struct apple_isp *isp)
{
	int retries;
	u32 status;

	isp_asc_write32(isp, ISP_ASC_EDPRCR, 0x2);

	isp_asc_write32(isp, ISP_ASC_PMGR_0, 0xff00ff);
	isp_asc_write32(isp, ISP_ASC_PMGR_1, 0xff00ff);
	isp_asc_write32(isp, ISP_ASC_PMGR_2, 0xff00ff);
	isp_asc_write32(isp, ISP_ASC_PMGR_3, 0xff00ff);

	isp_asc_write32(isp, ISP_ASC_IRQ_MASK_0, 0xffffffff);
	isp_asc_write32(isp, ISP_ASC_IRQ_MASK_1, 0xffffffff);
	isp_asc_write32(isp, ISP_ASC_IRQ_MASK_2, 0xffffffff);
	isp_asc_write32(isp, ISP_ASC_IRQ_MASK_3, 0xffffffff);
	isp_asc_write32(isp, ISP_ASC_IRQ_MASK_4, 0xffffffff);
	isp_asc_write32(isp, ISP_ASC_IRQ_MASK_5, 0xffffffff);

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		status = isp_asc_read32(isp, ISP_ASC_STATUS);
		if (!((status & 0x3) == 0)) {
			isp_dbg(isp, "%d: coproc in WFI (status: 0x%x)\n",
				retries, status);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp, "coproc NOT in WFI (status: 0x%x)\n", status);
		return -ENODEV;
	}

	return 0;
}

static void isp_firmware_shutdown_stage1(struct apple_isp *isp)
{
	isp_asc_write32(isp, ISP_ASC_CONTROL, 0x0);
}

static int isp_firmware_boot_stage1(struct apple_isp *isp)
{
	int err, retries;

	err = isp_coproc_ready(isp);
	if (err < 0)
		return err;

	isp_core_write32(isp, ISP_CORE_CLOCK_EN, 0x1);

	isp_core_write32(isp, ISP_CORE_GPIO_0, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_1, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_2, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_3, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_4, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_5, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_6, 0x0);
	isp_core_write32(isp, ISP_CORE_GPIO_7, 0x0);

	isp_core_write32(isp, ISP_CORE_IRQ_ENABLE, 0x0);

	isp_asc_write32(isp, ISP_ASC_CONTROL, 0x0);
	isp_asc_write32(isp, ISP_ASC_CONTROL, 0x10);

	/* Wait for ISP_CORE_GPIO_7 to 0x0 -> 0x8042006 */
	isp_core_write32(isp, ISP_CORE_GPIO_7, 0x0);
	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_core_read32(isp, ISP_CORE_GPIO_7);
		if (val == 0x8042006) {
			isp_dbg(isp,
				"got first magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp,
			"never received first magic number from firmware\n");
		return -ENODEV;
	}

	return 0;
}

static void isp_firmware_shutdown_stage2(struct apple_isp *isp)
{
	isp_free_surface(isp, isp->data_surf);
	isp_free_surface(isp, isp->extra_surf);
	isp_free_surface(isp, isp->ipc_surf);
}

static int isp_firmware_boot_stage2(struct apple_isp *isp)
{
	struct isp_firmware_bootargs args;
	dma_addr_t args_iova;
	int err, retries;

	u32 num_ipc_chans = isp_core_read32(isp, ISP_CORE_GPIO_0);
	u32 args_offset = isp_core_read32(isp, ISP_CORE_GPIO_1);
	u32 extra_size = isp_core_read32(isp, ISP_CORE_GPIO_3);
	isp->num_ipc_chans = num_ipc_chans;

	if (!isp->num_ipc_chans) {
		dev_err(isp->dev, "No IPC channels found\n");
		return -ENODEV;
	}

	if (isp->num_ipc_chans != 7)
		dev_warn(isp->dev, "unexpected channel count (%d)\n",
			 num_ipc_chans);

	isp->ipc_surf = isp_alloc_surface_vmap(isp, ISP_FIRMWARE_IPC_SIZE);
	if (!isp->ipc_surf) {
		isp_err(isp, "failed to alloc surface for ipc\n");
		return -ENOMEM;
	}

	isp->extra_surf = isp_alloc_surface_vmap(isp, extra_size);
	if (!isp->extra_surf) {
		isp_err(isp, "failed to alloc surface for extra heap\n");
		goto free_ipc;
	}

	isp->data_surf = isp_alloc_surface_vmap(isp, ISP_FIRMWARE_DATA_SIZE);
	if (!isp->data_surf) {
		isp_err(isp, "failed to alloc surface for data files\n");
		goto free_extra;
	}

	args_iova = isp->ipc_surf->iova + args_offset + 0x40;
	isp->cmd_iova = args_iova + sizeof(args) + 0x40;

	memset(&args, 0, sizeof(args));
	args.ipc_iova = isp->ipc_surf->iova;
	args.ipc_size = isp->ipc_surf->size;
	args.unk_size = 0x1800000;
	args.unk_inv = 0x10000000 - args.unk_size;
	args.extra_iova = isp->extra_surf->iova;
	args.extra_size = isp->extra_surf->size;
	args.unk4 = 0x1;
	args.unk5 = 0x40;
	args.unk7 = 0x1;
	args.unk_iova1 = args_iova + ISP_FIRMWARE_BOOTARGS_SIZE - 0xc;
	args.unk9 = 0x3;
	isp_iowrite(isp, args_iova, &args, sizeof(args));

	isp_core_write32(isp, ISP_CORE_GPIO_0, args_iova);
	isp_core_write32(isp, ISP_CORE_GPIO_1, 0x0);

	/* Wait for ISP_CORE_GPIO_7 to 0xf7fbdff9 -> 0x8042006 */
	isp_core_write32(isp, ISP_CORE_GPIO_7, 0xf7fbdff9);

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_core_read32(isp, ISP_CORE_GPIO_7);
		if (val == 0x8042006) {
			isp_dbg(isp,
				"got second magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp,
			"never received second magic number from firmware\n");
		err = -ENODEV;
		goto free_file;
	}

	return 0;

free_file:
	isp_free_surface(isp, isp->data_surf);
free_extra:
	isp_free_surface(isp, isp->extra_surf);
free_ipc:
	isp_free_surface(isp, isp->ipc_surf);
	return err;
}

static inline struct isp_channel *isp_get_chan_index(struct apple_isp *isp,
						     const char *name)
{
	for (int i = 0; i < isp->num_ipc_chans; i++) {
		if (!strcasecmp(isp->ipc_chans[i]->name, name))
			return isp->ipc_chans[i];
	}
	return NULL;
}

static void isp_free_channel_info(struct apple_isp *isp)
{
	for (int i = 0; i < isp->num_ipc_chans; i++) {
		struct isp_channel *chan = isp->ipc_chans[i];
		if (!chan)
			continue;
		kfree(chan->name);
		kfree(chan);
		isp->ipc_chans[i] = NULL;
	}
	kfree(isp->ipc_chans);
	isp->ipc_chans = NULL;
}

static int isp_fill_channel_info(struct apple_isp *isp)
{
	u32 table_iova = isp_core_read32(isp, ISP_CORE_GPIO_0);

	isp->ipc_chans = kcalloc(isp->num_ipc_chans,
				 sizeof(struct isp_channel *), GFP_KERNEL);
	if (!isp->ipc_chans)
		goto out;

	for (int i = 0; i < isp->num_ipc_chans; i++) {
		struct isp_chan_desc desc;
		dma_addr_t desc_iova = table_iova + (i * sizeof(desc));
		struct isp_channel *chan =
			kzalloc(sizeof(struct isp_channel), GFP_KERNEL);
		if (!chan)
			goto out;
		isp->ipc_chans[i] = chan;

		isp_ioread(isp, desc_iova, &desc, sizeof(desc));
		chan->name = kstrdup(desc.name, GFP_KERNEL);
		chan->type = desc.type;
		chan->src = desc.src;
		chan->doorbell = 1 << chan->src;
		chan->num = desc.num;
		chan->size = desc.num * ISP_IPC_MESSAGE_SIZE;
		chan->iova = desc.iova;
		chan->cursor = 0;
		spin_lock_init(&chan->lock);

		if ((chan->type != ISP_IPC_CHAN_TYPE_COMMAND) &&
		    (chan->type != ISP_IPC_CHAN_TYPE_REPLY) &&
		    (chan->type != ISP_IPC_CHAN_TYPE_REPORT)) {
			isp_err(isp, "invalid ipc chan type (%d)\n",
				chan->type);
			goto out;
		}

		isp_dbg(isp, "chan: %s type: %d src: %d num: %d iova: 0x%llx\n",
			chan->name, chan->type, chan->src, chan->num,
			chan->iova);
	}

	isp->chan_tm = isp_get_chan_index(isp, "TERMINAL");
	isp->chan_io = isp_get_chan_index(isp, "IO");
	isp->chan_dg = isp_get_chan_index(isp, "DEBUG");
	isp->chan_bh = isp_get_chan_index(isp, "BUF_H2T");
	isp->chan_bt = isp_get_chan_index(isp, "BUF_T2H");
	isp->chan_sm = isp_get_chan_index(isp, "SHAREDMALLOC");
	isp->chan_it = isp_get_chan_index(isp, "IO_T2H");

	if (!isp->chan_tm || !isp->chan_io || !isp->chan_dg || !isp->chan_bh ||
	    !isp->chan_bt || !isp->chan_sm || !isp->chan_it) {
		isp_err(isp, "did not find all of the required ipc chans\n");
		goto out;
	}

	isp->chan_tm->ops = &tm_ops;
	isp->chan_sm->ops = &sm_ops;
	isp->chan_bt->ops = &bt_ops;

	return 0;
out:
	isp_free_channel_info(isp);
	return -ENOMEM;
}

static void isp_firmware_shutdown_stage3(struct apple_isp *isp)
{
	isp_free_channel_info(isp);
}

static int isp_firmware_boot_stage3(struct apple_isp *isp)
{
	int err, retries;

	err = isp_fill_channel_info(isp);
	if (err < 0)
		return err;

	/* Mask the command channels to prepare for submission */
	for (int i = 0; i < isp->num_ipc_chans; i++) {
		struct isp_channel *chan = isp->ipc_chans[i];
		if (chan->type != ISP_IPC_CHAN_TYPE_COMMAND)
			continue;
		for (int j = 0; j < chan->num; j++) {
			struct isp_message msg;
			dma_addr_t msg_iova = chan->iova + (j * sizeof(msg));

			memset(&msg, 0, sizeof(msg));
			msg.arg0 = ISP_IPC_FLAG_ACK;
			isp_iowrite(isp, msg_iova, &msg, sizeof(msg));
		}
	}

	/* Wait for ISP_CORE_GPIO_3 to 0x8042006 -> 0x0 */
	isp_core_write32(isp, ISP_CORE_GPIO_3, 0x8042006);

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_core_read32(isp, ISP_CORE_GPIO_3);
		if (val == 0x0) {
			isp_dbg(isp,
				"got third magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp,
			"never received third magic number from firmware\n");
		isp_free_channel_info(isp);
		return -ENODEV;
	}

	isp_dbg(isp, "firmware booted!\n");

	return 0;
}

static int isp_stop_command_processor(struct apple_isp *isp)
{
	int retries;

	/* Wait for ISP_CORE_GPIO_0 to 0xf7fbdff9 -> 0x8042006 */
	isp_core_write32(isp, ISP_CORE_GPIO_0, 0xf7fbdff9);

	/* Their CISP_CMD_STOP implementation is buggy */
	isp_cmd_suspend(isp);

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_core_read32(isp, ISP_CORE_GPIO_0);
		if (val == 0x8042006) {
			isp_dbg(isp, "got magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp, "never received magic number from firmware\n");
		return -ENODEV;
	}

	return 0;
}

static int isp_start_command_processor(struct apple_isp *isp)
{
	int err;

	err = isp_cmd_print_enable(isp, 1);
	if (err)
		return err;

	err = isp_cmd_set_isp_pmu_base(isp, isp->hw->pmu_base);
	if (err)
		return err;

	err = isp_cmd_set_dsid_clr_req_base2(
		isp, isp->hw->dsid_clr_base0, isp->hw->dsid_clr_base1,
		isp->hw->dsid_clr_base2, isp->hw->dsid_clr_base3,
		isp->hw->dsid_clr_range0, isp->hw->dsid_clr_range1,
		isp->hw->dsid_clr_range2, isp->hw->dsid_clr_range3);
	if (err)
		return err;

	err = isp_cmd_pmp_ctrl_set(
		isp, isp->hw->clock_scratch, isp->hw->clock_base,
		isp->hw->clock_bit, isp->hw->clock_size,
		isp->hw->bandwidth_scratch, isp->hw->bandwidth_base,
		isp->hw->bandwidth_bit, isp->hw->bandwidth_size);
	if (err)
		return err;

	err = isp_cmd_start(isp, 0);
	if (err)
		return err;

	/* Now we can access CISP_CMD_CH_* commands */

	return 0;
}

static void isp_collect_gc_surface(struct apple_isp *isp)
{
	struct isp_surf *tmp, *surf;
	list_for_each_entry_safe_reverse(surf, tmp, &isp->gc, head) {
		isp_dbg(isp, "freeing iova: 0x%llx size: 0x%llx virt: %pS\n",
			surf->iova, surf->size, (void *)surf->virt);
		isp_free_surface(isp, surf);
	}
}

static int isp_firmware_boot(struct apple_isp *isp)
{
	int err;

	err = isp_firmware_boot_stage1(isp);
	if (err < 0) {
		isp_err(isp, "failed firmware boot stage 1: %d\n", err);
		goto garbage_collect;
	}

	err = isp_firmware_boot_stage2(isp);
	if (err < 0) {
		isp_err(isp, "failed firmware boot stage 2: %d\n", err);
		goto shutdown_stage1;
	}

	err = isp_firmware_boot_stage3(isp);
	if (err < 0) {
		isp_err(isp, "failed firmware boot stage 3: %d\n", err);
		goto shutdown_stage2;
	}

	err = isp_enable_irq(isp);
	if (err < 0) {
		isp_err(isp, "failed to enable interrupts: %d\n", err);
		goto shutdown_stage3;
	}

	err = isp_start_command_processor(isp);
	if (err < 0) {
		isp_err(isp, "failed to start command processor: %d\n", err);
		goto disable_irqs;
	}

	flush_workqueue(isp->wq);

	return 0;

disable_irqs:
	isp_disable_irq(isp);
shutdown_stage3:
	isp_firmware_shutdown_stage3(isp);
shutdown_stage2:
	isp_firmware_shutdown_stage2(isp);
shutdown_stage1:
	isp_firmware_shutdown_stage1(isp);
garbage_collect:
	isp_collect_gc_surface(isp);
	return err;
}

static void isp_firmware_shutdown(struct apple_isp *isp)
{
	flush_workqueue(isp->wq);
	isp_stop_command_processor(isp);
	isp_disable_irq(isp);
	isp_firmware_shutdown_stage3(isp);
	isp_firmware_shutdown_stage2(isp);
	isp_firmware_shutdown_stage1(isp);
	isp_collect_gc_surface(isp);
}

int apple_isp_firmware_boot(struct apple_isp *isp)
{
	int err;

	/* Needs to be power cycled for IOMMU to behave correctly */
	err = pm_runtime_resume_and_get(isp->dev);
	if (err < 0) {
		dev_err(isp->dev, "failed to enable power: %d\n", err);
		return err;
	}

	err = isp_firmware_boot(isp);
	if (err) {
		dev_err(isp->dev, "failed to boot firmware: %d\n", err);
		pm_runtime_put_sync(isp->dev);
		return err;
	}

	return 0;
}

void apple_isp_firmware_shutdown(struct apple_isp *isp)
{
	isp_firmware_shutdown(isp);
	pm_runtime_put_sync(isp->dev);
}
