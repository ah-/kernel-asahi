// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/align.h>
#include <linux/apple-mailbox.h>
#include <linux/soc/apple/rtkit.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "dcpep.h"
#include "dcp.h"
#include "parser.h"

struct apple_dcp;

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)

/* Register defines used in bandwidth setup structure */
#define REG_SCRATCH (0x14)
#define REG_SCRATCH_T600X (0x988)
#define REG_DOORBELL (0x0)
#define REG_DOORBELL_BIT (2)

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

/* Limit on call stack depth (arbitrary). Some nesting is required */
#define DCP_MAX_CALL_DEPTH 8

typedef void (*dcp_callback_t)(struct apple_dcp *, void *, void *);

struct dcp_call_channel {
	dcp_callback_t callbacks[DCP_MAX_CALL_DEPTH];
	void *cookies[DCP_MAX_CALL_DEPTH];
	void *output[DCP_MAX_CALL_DEPTH];
	u16 end[DCP_MAX_CALL_DEPTH];

	/* Current depth of the call stack. Less than DCP_MAX_CALL_DEPTH */
	u8 depth;
};

struct dcp_cb_channel {
	u8 depth;
	void *output[DCP_MAX_CALL_DEPTH];
};

/* Temporary backing for a chunked transfer via setDCPAVPropStart/Chunk/End */
struct dcp_chunks {
	size_t length;
	void *data;
};

#define DCP_MAX_MAPPINGS (128) /* should be enough */
#define MAX_DISP_REGISTERS (7)

struct apple_dcp {
	struct device *dev;
	struct platform_device *piodma;
	struct apple_rtkit *rtk;
	struct apple_crtc *crtc;
	struct apple_connector *connector;

	/* DCP has crashed */
	bool crashed;

	/* clock rate request by dcp in */
	struct clk *clk;

	/* DCP shared memory */
	void *shmem;

	/* Coprocessor control register */
	void __iomem *coproc_reg;

	/* Display registers mappable to the DCP */
	struct resource *disp_registers[MAX_DISP_REGISTERS];
	unsigned int nr_disp_registers;

	/* Number of memory mappings made by the DCP, used as an ID */
	u32 nr_mappings;

	/* Indexed table of mappings */
	struct sg_table mappings[DCP_MAX_MAPPINGS];

	struct dcp_call_channel ch_cmd, ch_oobcmd;
	struct dcp_cb_channel ch_cb, ch_oobcb, ch_async;

	/* Active chunked transfer. There can only be one at a time. */
	struct dcp_chunks chunks;

	/* Queued swap. Owned by the DCP to avoid per-swap memory allocation */
	struct dcp_swap_submit_req swap;

	/* Current display mode */
	bool valid_mode;
	struct dcp_set_digital_out_mode_req mode;

	/* Is the DCP booted? */
	bool active;

	/* Modes valid for the connected display */
	struct dcp_display_mode *modes;
	unsigned int nr_modes;

	/* Attributes of the connected display */
	int width_mm, height_mm;

	/* Workqueue for sending vblank events when a dcp swap is not possible */
	struct work_struct vblank_wq;
};

/*
 * A channel is busy if we have sent a message that has yet to be
 * acked. The driver must not sent a message to a busy channel.
 */
static bool dcp_channel_busy(struct dcp_call_channel *ch)
{
	return (ch->depth != 0);
}

/* Get a call channel for a context */
static struct dcp_call_channel *
dcp_get_call_channel(struct apple_dcp *dcp, enum dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CMD:
	case DCP_CONTEXT_CB:
		return &dcp->ch_cmd;
	case DCP_CONTEXT_OOBCMD:
	case DCP_CONTEXT_OOBCB:
		return &dcp->ch_oobcmd;
	default:
		return NULL;
	}
}

/*
 * Get the context ID passed to the DCP for a command we push. The rule is
 * simple: callback contexts are used when replying to the DCP, command
 * contexts are used otherwise. That corresponds to a non/zero call stack
 * depth. This rule frees the caller from tracking the call context manually.
 */
static enum dcp_context_id dcp_call_context(struct apple_dcp *dcp, bool oob)
{
	u8 depth = oob ? dcp->ch_oobcmd.depth : dcp->ch_cmd.depth;

	if (depth)
		return oob ? DCP_CONTEXT_OOBCB : DCP_CONTEXT_CB;
	else
		return oob ? DCP_CONTEXT_OOBCMD : DCP_CONTEXT_CMD;
}

/* Get a callback channel for a context */
static struct dcp_cb_channel *dcp_get_cb_channel(struct apple_dcp *dcp,
						 enum dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CB:
		return &dcp->ch_cb;
	case DCP_CONTEXT_OOBCB:
		return &dcp->ch_oobcb;
	case DCP_CONTEXT_ASYNC:
		return &dcp->ch_async;
	default:
		return NULL;
	}
}

/* Get the start of a packet: after the end of the previous packet */
static u16 dcp_packet_start(struct dcp_call_channel *ch, u8 depth)
{
	if (depth > 0)
		return ch->end[depth - 1];
	else
		return 0;
}

/* Pushes and pops the depth of the call stack with safety checks */
static u8 dcp_push_depth(u8 *depth)
{
	u8 ret = (*depth)++;

	WARN_ON(ret >= DCP_MAX_CALL_DEPTH);
	return ret;
}

static u8 dcp_pop_depth(u8 *depth)
{
	WARN_ON((*depth) == 0);

	return --(*depth);
}

#define DCP_METHOD(tag, name) [name] = { #name, tag }

const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	DCP_METHOD("A000", dcpep_late_init_signal),
	DCP_METHOD("A029", dcpep_setup_video_limits),
	DCP_METHOD("A034", dcpep_update_notify_clients_dcp),
	DCP_METHOD("A357", dcpep_set_create_dfb),
	DCP_METHOD("A401", dcpep_start_signal),
	DCP_METHOD("A407", dcpep_swap_start),
	DCP_METHOD("A408", dcpep_swap_submit),
	DCP_METHOD("A410", dcpep_set_display_device),
	DCP_METHOD("A412", dcpep_set_digital_out_mode),
	DCP_METHOD("A443", dcpep_create_default_fb),
	DCP_METHOD("A454", dcpep_first_client_open),
	DCP_METHOD("A460", dcpep_set_display_refresh_properties),
	DCP_METHOD("A463", dcpep_flush_supports_power),
	DCP_METHOD("A468", dcpep_set_power_state),
};

/* Call a DCP function given by a tag */
static void dcp_push(struct apple_dcp *dcp, bool oob, enum dcpep_method method,
		     u32 in_len, u32 out_len, void *data, dcp_callback_t cb,
		     void *cookie)
{
	struct dcp_call_channel *ch = oob ? &dcp->ch_oobcmd : &dcp->ch_cmd;
	enum dcp_context_id context = dcp_call_context(dcp, oob);

	struct dcp_packet_header header = {
		.in_len = in_len,
		.out_len = out_len,

		/* Tag is reversed due to endianness of the fourcc */
		.tag[0] = dcp_methods[method].tag[3],
		.tag[1] = dcp_methods[method].tag[2],
		.tag[2] = dcp_methods[method].tag[1],
		.tag[3] = dcp_methods[method].tag[0],
	};

	u8 depth = dcp_push_depth(&ch->depth);
	u16 offset = dcp_packet_start(ch, depth);

	void *out = dcp->shmem + dcp_tx_offset(context) + offset;
	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	if (in_len > 0)
		memcpy(out_data, data, in_len);

	dev_dbg(dcp->dev, "---> %s: context %u, offset %u, depth %u\n",
		dcp_methods[method].name, context, offset, depth);

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);

	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT,
				 dcpep_msg(context, data_len, offset),
				 NULL, false);
}

#define DCP_THUNK_VOID(func, handle)                                           \
	static void func(struct apple_dcp *dcp, bool oob, dcp_callback_t cb,   \
			 void *cookie)                                         \
	{                                                                      \
		dcp_push(dcp, oob, handle, 0, 0, NULL, cb, cookie);            \
	}

#define DCP_THUNK_OUT(func, handle, T)                                         \
	static void func(struct apple_dcp *dcp, bool oob, dcp_callback_t cb,   \
			 void *cookie)                                         \
	{                                                                      \
		dcp_push(dcp, oob, handle, 0, sizeof(T), NULL, cb, cookie);    \
	}

#define DCP_THUNK_IN(func, handle, T)                                          \
	static void func(struct apple_dcp *dcp, bool oob, T *data,             \
			 dcp_callback_t cb, void *cookie)                      \
	{                                                                      \
		dcp_push(dcp, oob, handle, sizeof(T), 0, data, cb, cookie);    \
	}

#define DCP_THUNK_INOUT(func, handle, T_in, T_out)                             \
	static void func(struct apple_dcp *dcp, bool oob, T_in *data,          \
			 dcp_callback_t cb, void *cookie)                      \
	{                                                                      \
		dcp_push(dcp, oob, handle, sizeof(T_in), sizeof(T_out), data,  \
			 cb, cookie);                                          \
	}

DCP_THUNK_INOUT(dcp_swap_submit, dcpep_swap_submit, struct dcp_swap_submit_req,
		struct dcp_swap_submit_resp);

DCP_THUNK_INOUT(dcp_swap_start, dcpep_swap_start, struct dcp_swap_start_req,
		struct dcp_swap_start_resp);

DCP_THUNK_INOUT(dcp_set_power_state, dcpep_set_power_state,
		struct dcp_set_power_state_req,
		struct dcp_set_power_state_resp);

DCP_THUNK_INOUT(dcp_set_digital_out_mode, dcpep_set_digital_out_mode,
		struct dcp_set_digital_out_mode_req, u32);

DCP_THUNK_INOUT(dcp_set_display_device, dcpep_set_display_device, u32, u32);

DCP_THUNK_OUT(dcp_set_display_refresh_properties,
	      dcpep_set_display_refresh_properties, u32);

DCP_THUNK_OUT(dcp_late_init_signal, dcpep_late_init_signal, u32);
DCP_THUNK_IN(dcp_flush_supports_power, dcpep_flush_supports_power, u32);
DCP_THUNK_OUT(dcp_create_default_fb, dcpep_create_default_fb, u32);
DCP_THUNK_OUT(dcp_start_signal, dcpep_start_signal, u32);
DCP_THUNK_VOID(dcp_setup_video_limits, dcpep_setup_video_limits);
DCP_THUNK_VOID(dcp_set_create_dfb, dcpep_set_create_dfb);
DCP_THUNK_VOID(dcp_first_client_open, dcpep_first_client_open);

__attribute__((unused))
DCP_THUNK_IN(dcp_update_notify_clients_dcp, dcpep_update_notify_clients_dcp,
	     struct dcp_update_notify_clients_dcp);

/* Parse a callback tag "D123" into the ID 123. Returns -EINVAL on failure. */
static int dcp_parse_tag(char tag[4])
{
	u32 d[3];
	int i;

	if (tag[3] != 'D')
		return -EINVAL;

	for (i = 0; i < 3; ++i) {
		d[i] = (u32)(tag[i] - '0');

		if (d[i] > 9)
			return -EINVAL;
	}

	return d[0] + (d[1] * 10) + (d[2] * 100);
}

/* Ack a callback from the DCP */
static void dcp_ack(struct apple_dcp *dcp, enum dcp_context_id context)
{
	struct dcp_cb_channel *ch = dcp_get_cb_channel(dcp, context);

	dcp_pop_depth(&ch->depth);
	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT, dcpep_ack(context),
				 NULL, false);
}

/* DCP callback handlers */
static void dcpep_cb_nop(struct apple_dcp *dcp)
{
	/* No operation */
}

static u8 dcpep_cb_true(struct apple_dcp *dcp)
{
	return true;
}

static u8 dcpep_cb_false(struct apple_dcp *dcp)
{
	return false;
}

static u32 dcpep_cb_zero(struct apple_dcp *dcp)
{
	return 0;
}

/* HACK: moved here to avoid circular dependency between apple_drv and dcp */
void dcp_drm_crtc_vblank(struct apple_crtc *crtc)
{
	unsigned long flags;

	if (crtc->vsync_disabled)
		return;

	drm_crtc_handle_vblank(&crtc->base);

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		drm_crtc_vblank_put(&crtc->base);
		crtc->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

static void dcpep_cb_swap_complete(struct apple_dcp *dcp)
{
	dcp_drm_crtc_vblank(dcp->crtc);
}

static struct dcp_get_uint_prop_resp
dcpep_cb_get_uint_prop(struct apple_dcp *dcp, struct dcp_get_uint_prop_req *req)
{
	/* unimplemented for now */
	return (struct dcp_get_uint_prop_resp) {
		.value = 0
	};
}

/*
 * Callback to map a buffer allocated with allocate_buf for PIODMA usage.
 * PIODMA is separate from the main DCP and uses own IOVA space on a dedicated
 * stream of the display DART, rather than the expected DCP DART.
 *
 * XXX: This relies on dma_get_sgtable in concert with dma_map_sgtable, which
 * is a "fundamentally unsafe" operation according to the docs. And yet
 * everyone does it...
 */
static struct dcp_map_buf_resp
dcpep_cb_map_piodma(struct apple_dcp *dcp, struct dcp_map_buf_req *req)
{
	struct sg_table *map;
	int ret;

	if (req->buffer >= ARRAY_SIZE(dcp->mappings))
		goto reject;

	map = &dcp->mappings[req->buffer];

	if (!map->sgl)
		goto reject;

	/* Use PIODMA device instead of DCP to map against the right IOMMU. */
	ret = dma_map_sgtable(&dcp->piodma->dev, map, DMA_BIDIRECTIONAL, 0);

	if (ret)
		goto reject;

	return (struct dcp_map_buf_resp) {
		.dva = sg_dma_address(map->sgl)
	};

reject:
	dev_err(dcp->dev, "denying map of invalid buffer %llx for pidoma\n",
		req->buffer);
	return (struct dcp_map_buf_resp) {
		.ret = EINVAL
	};
}

/*
 * Allocate an IOVA contiguous buffer mapped to the DCP. The buffer need not be
 * physically contigiuous, however we should save the sgtable in case the
 * buffer needs to be later mapped for PIODMA.
 */
static struct dcp_allocate_buffer_resp
dcpep_cb_allocate_buffer(struct apple_dcp *dcp, struct dcp_allocate_buffer_req *req)
{
	struct dcp_allocate_buffer_resp resp = { 0 };
	void *buf;

	resp.dva_size = ALIGN(req->size, 4096);
	resp.mem_desc_id = ++dcp->nr_mappings;

	if (resp.mem_desc_id >= ARRAY_SIZE(dcp->mappings)) {
		dev_warn(dcp->dev, "DCP overflowed mapping table, ignoring");
		return resp;
	}

	buf = dma_alloc_coherent(dcp->dev, resp.dva_size, &resp.dva,
				 GFP_KERNEL);

	dma_get_sgtable(dcp->dev, &dcp->mappings[resp.mem_desc_id], buf,
			resp.dva, resp.dva_size);
	return resp;
}

/* Validate that the specified region is a display register */
static bool is_disp_register(struct apple_dcp *dcp, u64 start, u64 end)
{
	int i;

	for (i = 0; i < dcp->nr_disp_registers; ++i) {
		struct resource *r = dcp->disp_registers[i];

		if ((start >= r->start) && (end <= r->end))
			return true;
	}

	return false;
}

/*
 * Map contiguous physical memory into the DCP's address space. The firmware
 * uses this to map the display registers we advertise in
 * sr_map_device_memory_with_index, so we bounds check against that to guard
 * safe against malicious coprocessors.
 */
static struct dcp_map_physical_resp
dcpep_cb_map_physical(struct apple_dcp *dcp, struct dcp_map_physical_req *req)
{
	int size = ALIGN(req->size, 4096);

	if (!is_disp_register(dcp, req->paddr, req->paddr + size - 1)) {
		dev_err(dcp->dev, "refusing to map phys address %llx size %llx",
			req->paddr, req->size);
		return (struct dcp_map_physical_resp) { };
	}

	return (struct dcp_map_physical_resp) {
		.dva_size = size,
		.mem_desc_id = ++dcp->nr_mappings,
		.dva = dma_map_resource(dcp->dev, req->paddr, size,
					DMA_BIDIRECTIONAL, 0),
	};
}

static u64 dcpep_cb_get_frequency(struct apple_dcp *dcp)
{
	return clk_get_rate(dcp->clk);
}

static struct dcp_map_reg_resp
dcpep_cb_map_reg(struct apple_dcp *dcp, struct dcp_map_reg_req *req)
{
	if (req->index >= dcp->nr_disp_registers) {
		dev_warn(dcp->dev, "attempted to read invalid reg index %u",
			 req->index);

		return (struct dcp_map_reg_resp) {
			.ret = 1
		};
	} else {
		struct resource *rsrc = dcp->disp_registers[req->index];

		return (struct dcp_map_reg_resp) {
			.addr = rsrc->start,
			.length = resource_size(rsrc)
		};
	}
}

/* Chunked data transfer for property dictionaries */
static u8 dcpep_cb_prop_start(struct apple_dcp *dcp, u32 *length)
{
	if (dcp->chunks.data != NULL) {
		dev_warn(dcp->dev, "ignoring spurious transfer start\n");
		return false;
	}

	dcp->chunks.length = *length;
	dcp->chunks.data = devm_kzalloc(dcp->dev, *length, GFP_KERNEL);

	if (!dcp->chunks.data) {
		dev_warn(dcp->dev, "failed to allocate chunks\n");
		return false;
	}

	return true;
}

static u8 dcpep_cb_prop_chunk(struct apple_dcp *dcp,
			      struct dcp_set_dcpav_prop_chunk_req *req)
{
	if (!dcp->chunks.data) {
		dev_warn(dcp->dev, "ignoring spurious chunk\n");
		return false;
	}

	if (req->offset + req->length > dcp->chunks.length) {
		dev_warn(dcp->dev, "ignoring overflowing chunk\n");
		return false;
	}

	memcpy(dcp->chunks.data + req->offset, req->data, req->length);
	return true;
}

static void dcp_set_dimensions(struct apple_dcp *dcp)
{
	int i;

	/* Set the connector info */
	if (dcp->connector) {
		struct drm_connector *connector = &dcp->connector->base;

		mutex_lock(&connector->dev->mode_config.mutex);
		connector->display_info.width_mm = dcp->width_mm;
		connector->display_info.height_mm = dcp->height_mm;
		mutex_unlock(&connector->dev->mode_config.mutex);
	}

	/*
	 * Fix up any probed modes. Modes are created when parsing
	 * TimingElements, dimensions are calculated when parsing
	 * DisplayAttributes, and TimingElements may be sent first
	 */
	for (i = 0; i < dcp->nr_modes; ++i) {
		dcp->modes[i].mode.width_mm = dcp->width_mm;
		dcp->modes[i].mode.height_mm = dcp->height_mm;
	}
}

static bool dcpep_process_chunks(struct apple_dcp *dcp,
				 struct dcp_set_dcpav_prop_end_req *req)
{
	struct dcp_parse_ctx ctx;
	int ret;

	if (!dcp->chunks.data) {
		dev_warn(dcp->dev, "ignoring spurious end\n");
		return false;
	}

	ret = parse(dcp->chunks.data, dcp->chunks.length, &ctx);

	if (ret) {
		dev_warn(dcp->dev, "bad header on dcpav props\n");
		return false;
	}

	if (!strcmp(req->key, "TimingElements")) {
		dcp->modes = enumerate_modes(&ctx, &dcp->nr_modes,
					     dcp->width_mm, dcp->height_mm);

		if (IS_ERR(dcp->modes)) {
			dev_warn(dcp->dev, "failed to parse modes\n");
			dcp->modes = NULL;
			dcp->nr_modes = 0;
			return false;
		}
	} else if (!strcmp(req->key, "DisplayAttributes")) {
		ret = parse_display_attributes(&ctx, &dcp->width_mm,
					       &dcp->height_mm);

		if (ret) {
			dev_warn(dcp->dev, "failed to parse display attribs\n");
			return false;
		}

		dcp_set_dimensions(dcp);
	}

	return true;
}

static u8 dcpep_cb_prop_end(struct apple_dcp *dcp,
			    struct dcp_set_dcpav_prop_end_req *req)
{
	u8 resp = dcpep_process_chunks(dcp, req);

	/* Reset for the next transfer */
	devm_kfree(dcp->dev, dcp->chunks.data);
	dcp->chunks.data = NULL;

	return resp;
}

/* Boot sequence */
static void boot_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_cb_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];

	*succ = true;
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static void boot_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_set_display_refresh_properties(dcp, false, boot_done, NULL);
}

static void boot_4(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_late_init_signal(dcp, false, boot_5, NULL);
}

static void boot_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 v_true = true;

	dcp_flush_supports_power(dcp, false, &v_true, boot_4, NULL);
}

static void boot_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_setup_video_limits(dcp, false, boot_3, NULL);
}

static void boot_1_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_create_default_fb(dcp, false, boot_2, NULL);
}

/* Use special function signature to defer the ACK */
static bool dcpep_cb_boot_1(struct apple_dcp *dcp, void *out, void *in)
{
	dcp_set_create_dfb(dcp, false, boot_1_5, NULL);
	return false;
}

static struct dcp_rt_bandwidth dcpep_cb_rt_bandwidth(struct apple_dcp *dcp)
{
	if (dcp->disp_registers[5] && dcp->disp_registers[6])
		return (struct dcp_rt_bandwidth) {
			.reg_scratch = dcp->disp_registers[5]->start + REG_SCRATCH,
			.reg_doorbell = dcp->disp_registers[6]->start + REG_DOORBELL,
			.doorbell_bit = REG_DOORBELL_BIT,

			.padding[3] = 0x4, // XXX: required by 11.x firmware
		};
	else if (dcp->disp_registers[4])
		return (struct dcp_rt_bandwidth) {
			.reg_scratch = dcp->disp_registers[4]->start + REG_SCRATCH_T600X,
			.reg_doorbell = 0,
			.doorbell_bit = 0,
		};
	else
		return (struct dcp_rt_bandwidth) {
			.reg_scratch = 0,
			.reg_doorbell = 0,
			.doorbell_bit = 0,
		};
}

/* Callback to get the current time as milliseconds since the UNIX epoch */
static u64 dcpep_cb_get_time(struct apple_dcp *dcp)
{
	return ktime_to_ms(ktime_get_real());
}

/*
 * Helper to send a DRM hotplug event. The DCP is accessed from a single
 * (RTKit) thread. To handle hotplug callbacks, we need to call
 * drm_kms_helper_hotplug_event, which does an atomic commit (via DCP) and
 * waits for vblank (a DCP callback). That means we deadlock if we call from
 * the RTKit thread! Instead, move the call to another thread via a workqueue.
 */
void dcp_hotplug(struct work_struct *work)
{
	struct apple_connector *connector;
	struct drm_device *dev;

	connector = container_of(work, struct apple_connector, hotplug_wq);
	dev = connector->base.dev;

	/*
	 * DCP defers link training until we set a display mode. But we set
	 * display modes from atomic_flush, so userspace needs to trigger a
	 * flush, or the CRTC gets no signal.
	 */
	if (connector->connected) {
		drm_connector_set_link_status_property(
			&connector->base, DRM_MODE_LINK_STATUS_BAD);
	}

	if (dev && dev->registered)
		drm_kms_helper_hotplug_event(dev);
}
EXPORT_SYMBOL_GPL(dcp_hotplug);

static void dcpep_cb_hotplug(struct apple_dcp *dcp, u64 *connected)
{
	struct apple_connector *connector = dcp->connector;

	/* Hotplug invalidates mode. DRM doesn't always handle this. */
	dcp->valid_mode = false;

	if (connector) {
		connector->connected = !!(*connected);
		schedule_work(&connector->hotplug_wq);
	}
}

/*
 * Helper to send a DRM vblank event. We do not know how call swap_submit_dcp
 * without surfaces. To avoid timeouts in drm_atomic_helper_wait_for_vblanks
 * send a vblank event via a workqueue.
 */
static void dcp_delayed_vblank(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	mdelay(5);
	dcp_drm_crtc_vblank(dcp->crtc);
}


#define DCPEP_MAX_CB (1000)

/*
 * Define type-safe trampolines. Define typedefs to enforce type-safety on the
 * input data (so if the types don't match, gcc errors out).
 */

#define TRAMPOLINE_VOID(func, handler)                                         \
	static bool func(struct apple_dcp *dcp, void *out, void *in)           \
	{                                                                      \
		dev_dbg(dcp->dev, "received callback %s\n", #handler);         \
		handler(dcp);                                                  \
		return true;                                                   \
	}

#define TRAMPOLINE_IN(func, handler, T_in)                                     \
	typedef void (*callback_##name)(struct apple_dcp *, T_in *);           \
                                                                               \
	static bool func(struct apple_dcp *dcp, void *out, void *in)           \
	{                                                                      \
		callback_##name cb = handler;                                  \
                                                                               \
		dev_dbg(dcp->dev, "received callback %s\n", #handler);         \
		cb(dcp, in);                                                   \
		return true;                                                   \
	}

#define TRAMPOLINE_INOUT(func, handler, T_in, T_out)                           \
	typedef T_out (*callback_##handler)(struct apple_dcp *, T_in *);       \
                                                                               \
	static bool func(struct apple_dcp *dcp, void *out, void *in)           \
	{                                                                      \
		T_out *typed_out = out;                                        \
		callback_##handler cb = handler;                               \
                                                                               \
		dev_dbg(dcp->dev, "received callback %s\n", #handler);         \
		*typed_out = cb(dcp, in);                                      \
		return true;                                                   \
	}

#define TRAMPOLINE_OUT(func, handler, T_out)                                   \
	static bool func(struct apple_dcp *dcp, void *out, void *in)           \
	{                                                                      \
		T_out *typed_out = out;                                        \
                                                                               \
		dev_dbg(dcp->dev, "received callback %s\n", #handler);         \
		*typed_out = handler(dcp);                                     \
		return true;                                                   \
	}

TRAMPOLINE_VOID(trampoline_nop, dcpep_cb_nop);
TRAMPOLINE_OUT(trampoline_true, dcpep_cb_true, u8);
TRAMPOLINE_OUT(trampoline_false, dcpep_cb_false, u8);
TRAMPOLINE_OUT(trampoline_zero, dcpep_cb_zero, u32);
TRAMPOLINE_VOID(trampoline_swap_complete, dcpep_cb_swap_complete);
TRAMPOLINE_INOUT(trampoline_get_uint_prop, dcpep_cb_get_uint_prop,
		 struct dcp_get_uint_prop_req, struct dcp_get_uint_prop_resp);
TRAMPOLINE_INOUT(trampoline_map_piodma, dcpep_cb_map_piodma,
		 struct dcp_map_buf_req, struct dcp_map_buf_resp);
TRAMPOLINE_INOUT(trampoline_allocate_buffer, dcpep_cb_allocate_buffer,
		 struct dcp_allocate_buffer_req,
		 struct dcp_allocate_buffer_resp);
TRAMPOLINE_INOUT(trampoline_map_physical, dcpep_cb_map_physical,
		 struct dcp_map_physical_req, struct dcp_map_physical_resp);
TRAMPOLINE_INOUT(trampoline_map_reg, dcpep_cb_map_reg, struct dcp_map_reg_req,
		 struct dcp_map_reg_resp);
TRAMPOLINE_INOUT(trampoline_prop_start, dcpep_cb_prop_start, u32, u8);
TRAMPOLINE_INOUT(trampoline_prop_chunk, dcpep_cb_prop_chunk,
		 struct dcp_set_dcpav_prop_chunk_req, u8);
TRAMPOLINE_INOUT(trampoline_prop_end, dcpep_cb_prop_end,
		 struct dcp_set_dcpav_prop_end_req, u8);
TRAMPOLINE_OUT(trampoline_rt_bandwidth, dcpep_cb_rt_bandwidth,
	       struct dcp_rt_bandwidth);
TRAMPOLINE_OUT(trampoline_get_frequency, dcpep_cb_get_frequency, u64);
TRAMPOLINE_OUT(trampoline_get_time, dcpep_cb_get_time, u64);
TRAMPOLINE_IN(trampoline_hotplug, dcpep_cb_hotplug, u64);

bool (*const dcpep_cb_handlers[DCPEP_MAX_CB])(struct apple_dcp *, void *, void *) = {
	[0] = trampoline_true, /* did_boot_signal */
	[1] = trampoline_true, /* did_power_on_signal */
	[2] = trampoline_nop, /* will_power_off_signal */
	[3] = trampoline_rt_bandwidth,
	[100] = trampoline_nop, /* match_pmu_service */
	[101] = trampoline_zero, /* get_display_default_stride */
	[103] = trampoline_nop, /* set_boolean_property */
	[106] = trampoline_nop, /* remove_property */
	[107] = trampoline_true, /* create_provider_service */
	[108] = trampoline_true, /* create_product_service */
	[109] = trampoline_true, /* create_pmu_service */
	[110] = trampoline_true, /* create_iomfb_service */
	[111] = trampoline_false, /* create_backlight_service */
	[116] = dcpep_cb_boot_1,
	[118] = trampoline_false, /* is_dark_boot / is_waking_from_hibernate*/
	[120] = trampoline_false, /* read_edt_data */
	[122] = trampoline_prop_start,
	[123] = trampoline_prop_chunk,
	[124] = trampoline_prop_end,
	[201] = trampoline_map_piodma,
	[206] = trampoline_true, /* match_pmu_service_2 */
	[207] = trampoline_true, /* match_backlight_service */
	[208] = trampoline_get_time,
	[211] = trampoline_nop, /* update_backlight_factor_prop */
	[300] = trampoline_nop, /* pr_publish */
	[401] = trampoline_get_uint_prop,
	[404] = trampoline_nop, /* sr_set_uint_prop */
	[406] = trampoline_nop, /* set_fx_prop */
	[408] = trampoline_get_frequency,
	[411] = trampoline_map_reg,
	[413] = trampoline_true, /* sr_set_property_dict */
	[414] = trampoline_true, /* sr_set_property_int */
	[415] = trampoline_true, /* sr_set_property_bool */
	[451] = trampoline_allocate_buffer,
	[452] = trampoline_map_physical,
	[552] = trampoline_true, /* set_property_dict_0 */
	[561] = trampoline_true, /* set_property_dict */
	[563] = trampoline_true, /* set_property_int */
	[565] = trampoline_true, /* set_property_bool */
	[567] = trampoline_true, /* set_property_str */
	[574] = trampoline_zero, /* power_up_dart */
	[576] = trampoline_hotplug,
	[577] = trampoline_nop, /* powerstate_notify */
	[582] = trampoline_true, /* create_default_fb_surface */
	[589] = trampoline_swap_complete,
	[591] = trampoline_nop, /* swap_complete_intent_gated */
	[598] = trampoline_nop, /* find_swap_function_gated */
};

static void dcpep_handle_cb(struct apple_dcp *dcp, enum dcp_context_id context,
			    void *data, u32 length)
{
	struct device *dev = dcp->dev;
	struct dcp_packet_header *hdr = data;
	void *in, *out;
	int tag = dcp_parse_tag(hdr->tag);
	struct dcp_cb_channel *ch = dcp_get_cb_channel(dcp, context);
	u8 depth;

	if (tag < 0 || tag >= DCPEP_MAX_CB || !dcpep_cb_handlers[tag]) {
		dev_warn(dev, "received unknown callback %c%c%c%c\n",
			 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);
		return;
	}

	in = data + sizeof(*hdr);
	out = in + hdr->in_len;

	depth = dcp_push_depth(&ch->depth);
	ch->output[depth] = out;

	if (dcpep_cb_handlers[tag](dcp, out, in))
		dcp_ack(dcp, context);
}

static void dcpep_handle_ack(struct apple_dcp *dcp, enum dcp_context_id context,
			     void *data, u32 length)
{
	struct dcp_packet_header *header = data;
	struct dcp_call_channel *ch = dcp_get_call_channel(dcp, context);
	void *cookie;
	dcp_callback_t cb;

	if (!ch) {
		dev_warn(dcp->dev, "ignoring ack on context %X\n", context);
		return;
	}

	dcp_pop_depth(&ch->depth);

	cb = ch->callbacks[ch->depth];
	cookie = ch->cookies[ch->depth];

	if (cb)
		cb(dcp, data + sizeof(*header) + header->in_len, cookie);
}

static void dcpep_got_msg(struct apple_dcp *dcp, u64 message)
{
	enum dcp_context_id ctx_id;
	u16 offset;
	u32 length;
	int channel_offset;
	void *data;

	ctx_id = (message & DCPEP_CONTEXT_MASK) >> DCPEP_CONTEXT_SHIFT;
	offset = (message & DCPEP_OFFSET_MASK) >> DCPEP_OFFSET_SHIFT;
	length = (message >> DCPEP_LENGTH_SHIFT);

	channel_offset = dcp_channel_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(dcp->dev, "invalid context received %u", ctx_id);
		return;
	}

	data = dcp->shmem + channel_offset + offset;

	if (message & DCPEP_ACK)
		dcpep_handle_ack(dcp, ctx_id, data, length);
	else
		dcpep_handle_cb(dcp, ctx_id, data, length);
}

/*
 * Callback for swap requests. If a swap failed, we'll never get a swap
 * complete event so we need to fake a vblank event early to avoid a hang.
 */

static void dcp_swapped(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_submit_resp *resp = data;

	if (resp->ret) {
		dev_err(dcp->dev, "swap failed! status %u\n", resp->ret);
		dcp_drm_crtc_vblank(dcp->crtc);
	}
}

static void dcp_swap_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_start_resp *resp = data;

	dcp->swap.swap.swap_id = resp->swap_id;

	dcp_swap_submit(dcp, false, &dcp->swap, dcp_swapped, NULL);
}

/*
 * DRM specifies rectangles as start and end coordinates.  DCP specifies
 * rectangles as a start coordinate and a width/height. Convert a DRM rectangle
 * to a DCP rectangle.
 */
static struct dcp_rect drm_to_dcp_rect(struct drm_rect *rect)
{
	return (struct dcp_rect) {
		.x = rect->x1,
		.y = rect->y1,
		.w = drm_rect_width(rect),
		.h = drm_rect_height(rect)
	};
}

static u32 drm_format_to_dcp(u32 drm)
{
	switch (drm) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return fourcc_code('A', 'R', 'G', 'B');

	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return fourcc_code('A', 'B', 'G', 'R');
	}

	pr_warn("DRM format %X not supported in DCP\n", drm);
	return 0;
}

int dcp_get_modes(struct drm_connector *connector)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = apple_connector->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	int i;

	for (i = 0; i < dcp->nr_modes; ++i) {
		mode = drm_mode_duplicate(dev, &dcp->modes[i].mode);

		if (!mode) {
			dev_err(dev->dev, "Failed to duplicate display mode\n");
			return 0;
		}

		drm_mode_probed_add(connector, mode);
	}

	return dcp->nr_modes;
}
EXPORT_SYMBOL_GPL(dcp_get_modes);

/* The user may own drm_display_mode, so we need to search for our copy */
static struct dcp_display_mode *lookup_mode(struct apple_dcp *dcp,
					    struct drm_display_mode *mode)
{
	int i;

	for (i = 0; i < dcp->nr_modes; ++i) {
		if (drm_mode_match(mode, &dcp->modes[i].mode,
				   DRM_MODE_MATCH_TIMINGS |
				   DRM_MODE_MATCH_CLOCK))
			return &dcp->modes[i];
	}

	return NULL;
}

int dcp_mode_valid(struct drm_connector *connector,
		   struct drm_display_mode *mode)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = apple_connector->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return lookup_mode(dcp, mode) ? MODE_OK : MODE_BAD;
}
EXPORT_SYMBOL_GPL(dcp_mode_valid);

/* Helpers to modeset and swap, used to flush */
static void do_swap(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_start_req start_req = { 0 };

	dcp_swap_start(dcp, false, &start_req, dcp_swap_started, NULL);
}

static void dcp_modeset(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_set_digital_out_mode(dcp, false, &dcp->mode, do_swap, NULL);
}

void dcp_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_plane *plane;
	struct drm_plane_state *new_state, *old_state;
	struct drm_crtc_state *crtc_state;
	struct dcp_swap_submit_req *req = &dcp->swap;
	int l;
	int has_surface = 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (WARN(dcp_channel_busy(&dcp->ch_cmd), "unexpected busy channel") ||
	    WARN(!dcp->connector->connected, "can't flush if disconnected")) {
		/* HACK: issue a delayed vblank event to avoid timeouts in
		 * drm_atomic_helper_wait_for_vblanks().
		 */
		schedule_work(&dcp->vblank_wq);
		return;
	}

	/* Reset to defaults */
	memset(req, 0, sizeof(*req));
	for (l = 0; l < SWAP_SURFACES; l++)
		req->surf_null[l] = true;

	for_each_oldnew_plane_in_state(state, plane, old_state, new_state, l) {
		struct drm_framebuffer *fb = new_state->fb;
		struct drm_rect src_rect;

		WARN_ON(l >= SWAP_SURFACES);

		req->swap.swap_enabled |= BIT(l);

		if (!new_state->fb) {
			if (old_state->fb)
				req->swap.swap_enabled |= DCP_REMOVE_LAYERS;

			continue;
		}
		req->surf_null[l] = false;
		has_surface = 1;

		// XXX: awful hack! race condition between a framebuffer unbind
		// getting swapped out and GEM unreferencing a framebuffer. If
		// we lose the race, the display gets IOVA faults and the DCP
		// crashes. We need to extend the lifetime of the
		// drm_framebuffer (and hence the GEM object) until after we
		// get a swap complete for the swap unbinding it.
		drm_framebuffer_get(fb);

		drm_rect_fp_to_int(&src_rect, &new_state->src);

		req->swap.src_rect[l] = drm_to_dcp_rect(&src_rect);
		req->swap.dst_rect[l] = drm_to_dcp_rect(&new_state->dst);

		req->surf_iova[l] = drm_fb_dma_get_gem_addr(fb, new_state, 0);

		req->surf[l] = (struct dcp_surface) {
			.format = drm_format_to_dcp(fb->format->format),
			.xfer_func = 13,
			.colorspace = 1,
			.stride = fb->pitches[0],
			.width = fb->width,
			.height = fb->height,
			.buf_size = fb->height * fb->pitches[0],
			.surface_id = req->swap.surf_ids[l],

			/* Only used for compressed or multiplanar surfaces */
			.pix_size = 1,
			.pel_w = 1,
			.pel_h = 1,
			.has_comp = 1,
			.has_planes = 1,
		};
	}

	/* These fields should be set together */
	req->swap.swap_completed = req->swap.swap_enabled;

	if (drm_atomic_crtc_needs_modeset(crtc_state) || !dcp->valid_mode) {
		struct dcp_display_mode *mode;
		u32 handle = 2;

		mode = lookup_mode(dcp, &crtc_state->mode);
		if (!mode) {
			dev_warn(dcp->dev, "no match for " DRM_MODE_FMT,
				 DRM_MODE_ARG(&crtc_state->mode));
			schedule_work(&dcp->vblank_wq);
			return;
		}

		dcp->mode = (struct dcp_set_digital_out_mode_req) {
			.color_mode_id = mode->color_mode_id,
			.timing_mode_id = mode->timing_mode_id
		};

		dcp->valid_mode = true;

		dcp_set_display_device(dcp, false, &handle, dcp_modeset, NULL);
	}
	else if (!has_surface) {
		dev_warn(dcp->dev, "can't flush without surfaces, vsync:%d", dcp->crtc->vsync_disabled);
		/* HACK: issue a delayed vblank event to avoid timeouts in
		 * drm_atomic_helper_wait_for_vblanks(). It's currently unkown
		 * if and how DCP supports swaps without attached surfaces.
		 */
		schedule_work(&dcp->vblank_wq);
	} else
		do_swap(dcp, NULL, NULL);
}
EXPORT_SYMBOL_GPL(dcp_flush);

bool dcp_is_initialized(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->active;
}
EXPORT_SYMBOL_GPL(dcp_is_initialized);

static void init_done(struct apple_dcp *dcp, void *out, void *cookie)
{
}

static void init_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_set_power_state_req req = {
		.unklong = 1,
	};
	dcp_set_power_state(dcp, false, &req, init_done, NULL);
}

static void init_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_first_client_open(dcp, false, init_3, NULL);
}

static void dcp_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	dev_info(dcp->dev, "DCP booted\n");

	init_2(dcp, data, cookie);

	dcp->active = true;
}

static void dcp_got_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	enum dcpep_type type = (message >> DCPEP_TYPE_SHIFT) & DCPEP_TYPE_MASK;

	WARN_ON(endpoint != DCP_ENDPOINT);

	if (type == DCPEP_TYPE_INITIALIZED)
		dcp_start_signal(dcp, false, dcp_started, NULL);
	else if (type == DCPEP_TYPE_MESSAGE)
		dcpep_got_msg(dcp, message);
	else
		dev_warn(dcp->dev, "Ignoring unknown message %llx\n", message);
}

static void dcp_rtk_crashed(void *cookie)
{
	struct apple_dcp *dcp = cookie;

	dcp->crashed = true;
	dev_err(dcp->dev, "DCP has crashed");
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->iova) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain, bfr->iova & 0xFFFFFFFF);
		if (!phy_addr)
			return -ENOMEM;

		// TODO: verify phy_addr, cache attribute
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(dcp->dev, "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx",
			(uintptr_t)bfr->iova, (uintptr_t)phy_addr, (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size, &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		dev_info(dcp->dev, "shmem_setup: iova: %lx, buffer: %lx",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void dcp_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static struct apple_rtkit_ops rtkit_ops = {
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_got_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

void dcp_link(struct platform_device *pdev, struct apple_crtc *crtc,
	      struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	dcp->crtc = crtc;
	dcp->connector = connector;

	/* Dimensions might already be parsed */
	dcp_set_dimensions(dcp);
}
EXPORT_SYMBOL_GPL(dcp_link);

static struct platform_device *dcp_get_dev(struct device *dev, const char *name)
{
	struct device_node *node = of_get_child_by_name(dev->of_node, name);

	if (!node)
		return NULL;

	return of_find_device_by_node(node);
}

static int dcp_get_disp_regs(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int count = pdev->num_resources - 1;
	int i;

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		dcp->disp_registers[i] =
			platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
	}

	dcp->nr_disp_registers = count;
	return 0;
}

static int dcp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	dma_addr_t shmem_iova;
	u32 cpu_ctrl;
	int ret;

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	platform_set_drvdata(pdev, dcp);
	dcp->dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;

	dcp->coproc_reg = devm_platform_ioremap_resource_byname(pdev, "coproc");
	if (IS_ERR(dcp->coproc_reg))
		return PTR_ERR(dcp->coproc_reg);

	of_platform_default_populate(dev->of_node, NULL, dev);

	dcp->piodma = dcp_get_dev(dev, "piodma");
	if (!dcp->piodma) {
		dev_err(dev, "failed to find piodma\n");
		return -ENODEV;
	}

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		return ret;
	}

	dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk))
		return dev_err_probe(dev, PTR_ERR(dcp->clk), "Unable to find clock\n");

	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);

	cpu_ctrl = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
		       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	dcp->rtk = devm_apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk))
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to intialize RTKit");

	ret = apple_rtkit_wake(dcp->rtk);
	if (ret)
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to boot RTKit: %d", ret);

	apple_rtkit_start_ep(dcp->rtk, DCP_ENDPOINT);

	dcp->shmem = dma_alloc_coherent(dev, DCP_SHMEM_SIZE, &shmem_iova,
					GFP_KERNEL);

	apple_rtkit_send_message(dcp->rtk, DCP_ENDPOINT,
				 dcpep_set_shmem(shmem_iova), NULL, false);

	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
static void dcp_platform_shutdown(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	struct dcp_set_power_state_req req = {
		/* defaults are ok */
	};

	dcp_set_power_state(dcp, false, &req, NULL, NULL);
}

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,dcp" },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.probe		= dcp_platform_probe,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
	},
};

module_platform_driver(apple_platform_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION("Apple Display Controller DRM driver");
MODULE_LICENSE("Dual MIT/GPL");
