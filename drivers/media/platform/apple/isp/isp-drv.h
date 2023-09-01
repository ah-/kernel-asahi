// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2023 Eileen Yoon <eyn@gmx.com> */

#ifndef __ISP_DRV_H__
#define __ISP_DRV_H__

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/spinlock.h>

#include <drm/drm_mm.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>

/* #define APPLE_ISP_DEBUG */
#define APPLE_ISP_DEVICE_NAME "apple-isp"

#define ISP_MAX_RESV_REGIONS  4
#define ISP_MAX_CHANNELS      6
#define ISP_IPC_MESSAGE_SIZE  64
#define ISP_IPC_FLAG_ACK      0x1
#define ISP_META_SIZE	      0x4640

struct isp_surf {
	struct drm_mm_node *mm;
	struct list_head head;
	u64 size;
	u32 num_pages;
	struct page **pages;
	struct sg_table sgt;
	dma_addr_t iova;
	void *virt;
	refcount_t refcount;
	bool gc;
};

struct isp_message {
	u64 arg0;
	u64 arg1;
	u64 arg2;
	u64 arg3;
	u64 arg4;
	u64 arg5;
	u64 arg6;
	u64 arg7;
} __packed;
static_assert(sizeof(struct isp_message) == ISP_IPC_MESSAGE_SIZE);

struct isp_channel {
	char *name;
	u32 type;
	u32 src;
	u32 num;
	u64 size;
	dma_addr_t iova;
	u32 doorbell;
	u32 cursor;
	spinlock_t lock;
	struct isp_message req;
	struct isp_message rsp;
	const struct isp_chan_ops *ops;
};

struct apple_isp_hw {
	u64 pmu_base;

	u64 dsid_clr_base0;
	u64 dsid_clr_base1;
	u64 dsid_clr_base2;
	u64 dsid_clr_base3;
	u32 dsid_clr_range0;
	u32 dsid_clr_range1;
	u32 dsid_clr_range2;
	u32 dsid_clr_range3;

	u64 clock_scratch;
	u64 clock_base;
	u8 clock_bit;
	u8 clock_size;
	u64 bandwidth_scratch;
	u64 bandwidth_base;
	u8 bandwidth_bit;
	u8 bandwidth_size;

	u32 stream_command;
	u32 stream_select;
	u32 ttbr;
	u32 stream_command_invalidate;
};

struct isp_resv {
	phys_addr_t phys;
	dma_addr_t iova;
	u64 size;
};

enum isp_sensor_id {
	ISP_IMX248_1820_01,
	ISP_IMX248_1822_02,
	ISP_IMX343_5221_02,
	ISP_IMX354_9251_02,
	ISP_IMX356_4820_01,
	ISP_IMX356_4820_02,
	ISP_IMX364_8720_01,
	ISP_IMX364_8723_01,
	ISP_IMX372_3820_01,
	ISP_IMX372_3820_02,
	ISP_IMX372_3820_11,
	ISP_IMX372_3820_12,
	ISP_IMX405_9720_01,
	ISP_IMX405_9721_01,
	ISP_IMX405_9723_01,
	ISP_IMX414_2520_01,
	ISP_IMX503_7820_01,
	ISP_IMX503_7820_02,
	ISP_IMX505_3921_01,
	ISP_IMX514_2820_01,
	ISP_IMX514_2820_02,
	ISP_IMX514_2820_03,
	ISP_IMX514_2820_04,
	ISP_IMX558_1921_01,
	ISP_IMX558_1922_02,
	ISP_IMX603_7920_01,
	ISP_IMX603_7920_02,
	ISP_IMX603_7921_01,
	ISP_IMX613_4920_01,
	ISP_IMX613_4920_02,
	ISP_IMX614_2921_01,
	ISP_IMX614_2921_02,
	ISP_IMX614_2922_02,
	ISP_IMX633_3622_01,
	ISP_IMX703_7721_01,
	ISP_IMX703_7722_01,
	ISP_IMX713_4721_01,
	ISP_IMX713_4722_01,
	ISP_IMX714_2022_01,
	ISP_IMX772_3721_01,
	ISP_IMX772_3721_11,
	ISP_IMX772_3722_01,
	ISP_IMX772_3723_01,
	ISP_IMX814_2123_01,
	ISP_IMX853_7622_01,
	ISP_IMX913_7523_01,
	ISP_VD56G0_6221_01,
	ISP_VD56G0_6222_01,
};

struct isp_format {
	enum isp_sensor_id id;
	u32 version;
	u32 num_presets;
	u32 preset;
	u32 width;
	u32 height;
	u32 x1;
	u32 y1;
	u32 x2;
	u32 y2;
	unsigned int num_planes;
	size_t plane_size[VB2_MAX_PLANES];
	size_t total_size;
};

struct apple_isp {
	struct device *dev;
	const struct apple_isp_hw *hw;

	int num_channels;
	struct isp_format fmts[ISP_MAX_CHANNELS];
	unsigned int current_ch;

	struct video_device vdev;
	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct vb2_queue vbq;
	struct mutex video_lock;
	unsigned int sequence;
	bool multiplanar;

	int pd_count;
	struct device **pd_dev;
	struct device_link **pd_link;

	int irq;

	void __iomem *asc;
	void __iomem *mbox;
	void __iomem *gpio;
	void __iomem *dart0;
	void __iomem *dart1;
	void __iomem *dart2;

	struct iommu_domain *domain;
	unsigned long shift;
	struct drm_mm iovad; /* TODO iova.c can't allocate bottom-up */
	struct mutex iovad_lock;

	struct isp_firmware {
		int count;
		struct isp_resv resv[ISP_MAX_RESV_REGIONS];
		struct isp_surf *heap;
	} fw;

	struct isp_surf *ipc_surf;
	struct isp_surf *extra_surf;
	struct isp_surf *data_surf;
	struct list_head gc;
	struct workqueue_struct *wq;

	int num_ipc_chans;
	struct isp_channel **ipc_chans;
	struct isp_channel *chan_tm; /* TERMINAL */
	struct isp_channel *chan_io; /* IO */
	struct isp_channel *chan_dg; /* DEBUG */
	struct isp_channel *chan_bh; /* BUF_H2T */
	struct isp_channel *chan_bt; /* BUF_T2H */
	struct isp_channel *chan_sm; /* SHAREDMALLOC */
	struct isp_channel *chan_it; /* IO_T2H */

	wait_queue_head_t wait;
	dma_addr_t cmd_iova;

	unsigned long state;
	spinlock_t buf_lock;
	struct list_head buffers;
};

struct isp_chan_ops {
	int (*handle)(struct apple_isp *isp, struct isp_channel *chan);
};

struct isp_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head link;
	struct isp_surf surfs[VB2_MAX_PLANES];
	struct isp_surf *meta;
};

#define to_isp_buffer(x) container_of((x), struct isp_buffer, vb)

enum {
	ISP_STATE_STREAMING,
	ISP_STATE_LOGGING,
};

#ifdef APPLE_ISP_DEBUG
#define isp_dbg(isp, fmt, ...) \
	dev_info((isp)->dev, "[%s] " fmt, __func__, ##__VA_ARGS__)
#else
#define isp_dbg(isp, fmt, ...) \
	dev_dbg((isp)->dev, "[%s] " fmt, __func__, ##__VA_ARGS__)
#endif

#define isp_err(isp, fmt, ...) \
	dev_err((isp)->dev, "[%s] " fmt, __func__, ##__VA_ARGS__)

#define isp_get_format(isp, ch)	    (&(isp)->fmts[(ch)])
#define isp_get_current_format(isp) (isp_get_format(isp, isp->current_ch))

#endif /* __ISP_DRV_H__ */
