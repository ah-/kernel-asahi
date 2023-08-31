// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2023 Eileen Yoon <eyn@gmx.com> */

#include <linux/firmware.h>

#include "isp-cam.h"
#include "isp-cmd.h"
#include "isp-fw.h"
#include "isp-iommu.h"

struct isp_setfile {
	u32 version;
	u32 magic;
	const char *path;
	size_t size;
};

struct isp_preset {
	u32 index;
	u32 width;
	u32 height;
	u32 x1;
	u32 y1;
	u32 x2;
	u32 y2;
	u32 orig_width;
	u32 orig_height;
};

// clang-format off
static const struct isp_setfile isp_setfiles[] = {
	[ISP_IMX248_1820_01] = {0x248, 0x18200103, "isp/1820_01XX.dat", 0x442c},
	[ISP_IMX248_1822_02] = {0x248, 0x18220201, "isp/1822_02XX.dat", 0x442c},
	[ISP_IMX343_5221_02] = {0x343, 0x52210211, "isp/5221_02XX.dat", 0x4870},
	[ISP_IMX354_9251_02] = {0x354, 0x92510208, "isp/9251_02XX.dat", 0xa5ec},
	[ISP_IMX356_4820_01] = {0x356, 0x48200107, "isp/4820_01XX.dat", 0x9324},
	[ISP_IMX356_4820_02] = {0x356, 0x48200206, "isp/4820_02XX.dat", 0x9324},
	[ISP_IMX364_8720_01] = {0x364, 0x87200103, "isp/8720_01XX.dat", 0x36ac},
	[ISP_IMX364_8723_01] = {0x364, 0x87230101, "isp/8723_01XX.dat", 0x361c},
	[ISP_IMX372_3820_01] = {0x372, 0x38200108, "isp/3820_01XX.dat", 0xfdb0},
	[ISP_IMX372_3820_02] = {0x372, 0x38200205, "isp/3820_02XX.dat", 0xfdb0},
	[ISP_IMX372_3820_11] = {0x372, 0x38201104, "isp/3820_11XX.dat", 0xfdb0},
	[ISP_IMX372_3820_12] = {0x372, 0x38201204, "isp/3820_12XX.dat", 0xfdb0},
	[ISP_IMX405_9720_01] = {0x405, 0x97200102, "isp/9720_01XX.dat", 0x92c8},
	[ISP_IMX405_9721_01] = {0x405, 0x97210102, "isp/9721_01XX.dat", 0x9818},
	[ISP_IMX405_9723_01] = {0x405, 0x97230101, "isp/9723_01XX.dat", 0x92c8},
	[ISP_IMX414_2520_01] = {0x414, 0x25200102, "isp/2520_01XX.dat", 0xa444},
	[ISP_IMX503_7820_01] = {0x503, 0x78200109, "isp/7820_01XX.dat", 0xb268},
	[ISP_IMX503_7820_02] = {0x503, 0x78200206, "isp/7820_02XX.dat", 0xb268},
	[ISP_IMX505_3921_01] = {0x505, 0x39210102, "isp/3921_01XX.dat", 0x89b0},
	[ISP_IMX514_2820_01] = {0x514, 0x28200108, "isp/2820_01XX.dat", 0xa198},
	[ISP_IMX514_2820_02] = {0x514, 0x28200205, "isp/2820_02XX.dat", 0xa198},
	[ISP_IMX514_2820_03] = {0x514, 0x28200305, "isp/2820_03XX.dat", 0xa198},
	[ISP_IMX514_2820_04] = {0x514, 0x28200405, "isp/2820_04XX.dat", 0xa198},
	[ISP_IMX558_1921_01] = {0x558, 0x19210106, "isp/1921_01XX.dat", 0xad40},
	[ISP_IMX558_1922_02] = {0x558, 0x19220201, "isp/1922_02XX.dat", 0xad40},
	[ISP_IMX603_7920_01] = {0x603, 0x79200109, "isp/7920_01XX.dat", 0xad2c},
	[ISP_IMX603_7920_02] = {0x603, 0x79200205, "isp/7920_02XX.dat", 0xad2c},
	[ISP_IMX603_7921_01] = {0x603, 0x79210104, "isp/7921_01XX.dat", 0xad90},
	[ISP_IMX613_4920_01] = {0x613, 0x49200108, "isp/4920_01XX.dat", 0x9324},
	[ISP_IMX613_4920_02] = {0x613, 0x49200204, "isp/4920_02XX.dat", 0x9324},
	[ISP_IMX614_2921_01] = {0x614, 0x29210107, "isp/2921_01XX.dat", 0xed6c},
	[ISP_IMX614_2921_02] = {0x614, 0x29210202, "isp/2921_02XX.dat", 0xed6c},
	[ISP_IMX614_2922_02] = {0x614, 0x29220201, "isp/2922_02XX.dat", 0xed6c},
	[ISP_IMX633_3622_01] = {0x633, 0x36220111, "isp/3622_01XX.dat", 0x100d4},
	[ISP_IMX703_7721_01] = {0x703, 0x77210106, "isp/7721_01XX.dat", 0x936c},
	[ISP_IMX703_7722_01] = {0x703, 0x77220106, "isp/7722_01XX.dat", 0xac20},
	[ISP_IMX713_4721_01] = {0x713, 0x47210107, "isp/4721_01XX.dat", 0x936c},
	[ISP_IMX713_4722_01] = {0x713, 0x47220109, "isp/4722_01XX.dat", 0x9218},
	[ISP_IMX714_2022_01] = {0x714, 0x20220107, "isp/2022_01XX.dat", 0xa198},
	[ISP_IMX772_3721_01] = {0x772, 0x37210106, "isp/3721_01XX.dat", 0xfdf8},
	[ISP_IMX772_3721_11] = {0x772, 0x37211106, "isp/3721_11XX.dat", 0xfe14},
	[ISP_IMX772_3722_01] = {0x772, 0x37220104, "isp/3722_01XX.dat", 0xfca4},
	[ISP_IMX772_3723_01] = {0x772, 0x37230106, "isp/3723_01XX.dat", 0xfca4},
	[ISP_IMX814_2123_01] = {0x814, 0x21230101, "isp/2123_01XX.dat", 0xed54},
	[ISP_IMX853_7622_01] = {0x853, 0x76220112, "isp/7622_01XX.dat", 0x247f8},
	[ISP_IMX913_7523_01] = {0x913, 0x75230107, "isp/7523_01XX.dat", 0x247f8},
	[ISP_VD56G0_6221_01] = {0xd56, 0x62210102, "isp/6221_01XX.dat", 0x1b80},
	[ISP_VD56G0_6222_01] = {0xd56, 0x62220102, "isp/6222_01XX.dat", 0x1b80},
};
// clang-format on

// one day we will do this intelligently
static const struct isp_preset isp_presets[] = {
	[ISP_IMX248_1820_01] = { 0, 1280, 720, 8, 8, 1280, 720, 1296, 736 },
};

static int isp_ch_get_sensor_id(struct apple_isp *isp, u32 ch)
{
	struct isp_format *fmt = isp_get_format(isp, ch);
	enum isp_sensor_id id;
	int err = 0;

	/* TODO need more datapoints to figure out the sub-versions
	 * Defaulting to 1st release for now, the calib files aren't too different.
	 */
	switch (fmt->version) {
	case 0x248:
		id = ISP_IMX248_1820_01;
		break;
	case 0x343:
		id = ISP_IMX343_5221_02;
		break;
	case 0x354:
		id = ISP_IMX354_9251_02;
		break;
	case 0x356:
		id = ISP_IMX356_4820_01;
		break;
	case 0x364:
		id = ISP_IMX364_8720_01;
		break;
	case 0x372:
		id = ISP_IMX372_3820_01;
		break;
	case 0x405:
		id = ISP_IMX405_9720_01;
		break;
	case 0x414:
		id = ISP_IMX414_2520_01;
		break;
	case 0x503:
		id = ISP_IMX503_7820_01;
		break;
	case 0x505:
		id = ISP_IMX505_3921_01;
		break;
	case 0x514:
		id = ISP_IMX514_2820_01;
		break;
	case 0x558:
		id = ISP_IMX558_1921_01;
		break;
	case 0x603:
		id = ISP_IMX603_7920_01;
		break;
	case 0x613:
		id = ISP_IMX613_4920_01;
		break;
	case 0x614:
		id = ISP_IMX614_2921_01;
		break;
	case 0x633:
		id = ISP_IMX633_3622_01;
		break;
	case 0x703:
		id = ISP_IMX703_7721_01;
		break;
	case 0x713:
		id = ISP_IMX713_4721_01;
		break;
	case 0x714:
		id = ISP_IMX714_2022_01;
		break;
	case 0x772:
		id = ISP_IMX772_3721_01;
		break;
	case 0x814:
		id = ISP_IMX814_2123_01;
		break;
	case 0x853:
		id = ISP_IMX853_7622_01;
		break;
	case 0x913:
		id = ISP_IMX913_7523_01;
		break;
	case 0xd56:
		id = ISP_VD56G0_6221_01;
		break;
	default:
		err = -EINVAL;
		break;
	}

	if (err)
		dev_err(isp->dev, "invalid sensor version: 0x%x\n",
			fmt->version);
	else
		fmt->id = id;

	return err;
}

static int isp_ch_cache_sensor_info(struct apple_isp *isp, u32 ch)
{
	struct isp_format *fmt = isp_get_format(isp, ch);
	int err = 0;

	struct cmd_ch_info *args; /* Too big to allocate on stack */
	args = kzalloc(sizeof(*args), GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	err = isp_cmd_ch_info_get(isp, ch, args);
	if (err)
		goto exit;

	dev_info(isp->dev, "found sensor %x %s on ch %d\n", args->version,
		 args->module_sn, ch);

	fmt->version = args->version;
	fmt->num_presets = args->num_presets;

	pr_info("apple-isp: ch: CISP_CMD_CH_INFO_GET: %d\n", ch);
	print_hex_dump(KERN_INFO, "apple-isp: ch: ", DUMP_PREFIX_NONE, 32, 4,
		       args, sizeof(*args), false);

	err = isp_ch_get_sensor_id(isp, ch);
	if (err || (fmt->id != ISP_IMX248_1820_01)) {
		dev_err(isp->dev,
			"ch %d: unsupported sensor. Please file a bug report with hardware info & dmesg trace.\n",
			ch);
		return -ENODEV;
	}

exit:
	kfree(args);

	return err;
}

static int isp_ch_get_camera_preset(struct apple_isp *isp, u32 ch, u32 ps)
{
	int err = 0;

	struct cmd_ch_camera_config *args; /* Too big to allocate on stack */
	args = kzalloc(sizeof(*args), GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	err = isp_cmd_ch_camera_config_get(isp, ch, ps, args);
	if (err)
		goto exit;

	pr_info("apple-isp: ps: CISP_CMD_CH_CAMERA_CONFIG_GET: %d\n", ps);
	print_hex_dump(KERN_INFO, "apple-isp: ps: ", DUMP_PREFIX_NONE, 32, 4,
		       args, sizeof(*args), false);

exit:
	kfree(args);

	return err;
}

static void isp_ch_dump_camera_presets(struct apple_isp *isp, u32 ch)
{
	struct isp_format *fmt = isp_get_format(isp, ch);
	for (u32 ps = 0; ps < fmt->num_presets; ps++) {
		isp_ch_get_camera_preset(isp, ch, ps);
	}
}

static int isp_ch_cache_camera_preset(struct apple_isp *isp, u32 ch)
{
	struct isp_format *fmt = isp_get_format(isp, ch);
	const struct isp_preset *preset = &isp_presets[fmt->id];
	size_t total_size;

	isp_ch_dump_camera_presets(isp, ch);

	fmt->preset = preset->index;

	fmt->width = preset->width;
	fmt->height = preset->height;

	fmt->x1 = preset->x1;
	fmt->y1 = preset->y1;
	fmt->x2 = preset->x2;
	fmt->y2 = preset->y2;

	/* I really fucking hope they all use NV12. */
	fmt->num_planes = 2;
	fmt->plane_size[0] = fmt->width * fmt->height;
	fmt->plane_size[1] = fmt->plane_size[0] / 2;

	total_size = 0;
	for (int i = 0; i < fmt->num_planes; i++)
		total_size += fmt->plane_size[i];
	fmt->total_size = total_size;

	return 0;
}

static int isp_ch_cache_camera_info(struct apple_isp *isp, u32 ch)
{
	int err;

	err = isp_ch_cache_sensor_info(isp, ch);
	if (err) {
		dev_err(isp->dev, "ch %d: failed to cache sensor info: %d\n",
			ch, err);
		return err;
	}

	err = isp_ch_cache_camera_preset(isp, ch);
	if (err) {
		dev_err(isp->dev, "ch %d: failed to cache camera preset: %d\n",
			ch, err);
		return err;
	}

	return 0;
}

static int isp_detect_camera(struct apple_isp *isp)
{
	int err;

	struct cmd_config_get args;
	memset(&args, 0, sizeof(args));

	err = isp_cmd_config_get(isp, &args);
	if (err)
		return err;

	pr_info("apple-isp: CISP_CMD_CONFIG_GET: \n");
	print_hex_dump(KERN_INFO, "apple-isp: ", DUMP_PREFIX_NONE, 32, 4, &args,
		       sizeof(args), false);

	if (!args.num_channels) {
		dev_err(isp->dev, "did not detect any channels\n");
		return -ENODEV;
	}

	if (args.num_channels > ISP_MAX_CHANNELS) {
		dev_warn(isp->dev, "found %d channels when maximum is %d\n",
			 args.num_channels, ISP_MAX_CHANNELS);
		args.num_channels = ISP_MAX_CHANNELS;
	}

	if (args.num_channels > 1) {
		dev_warn(
			isp->dev,
			"warning: driver doesn't support multiple channels. Please file a bug report with hardware info & dmesg trace.\n");
	}

	isp->num_channels = args.num_channels;
	isp->current_ch = 0;

	return isp_ch_cache_camera_info(isp, isp->current_ch); /* I told you */
}

int apple_isp_detect_camera(struct apple_isp *isp)
{
	int err;

	/* RPM must be enabled prior to calling this */
	err = apple_isp_firmware_boot(isp);
	if (err) {
		dev_err(isp->dev,
			"failed to boot firmware for initial sensor detection: %d\n",
			err);
		return -EPROBE_DEFER;
	}

	err = isp_detect_camera(isp);
	apple_isp_firmware_shutdown(isp);

	return err;
}

static int isp_ch_load_setfile(struct apple_isp *isp, u32 ch)
{
	struct isp_format *fmt = isp_get_format(isp, ch);
	const struct isp_setfile *setfile = &isp_setfiles[fmt->id];
	const struct firmware *fw;
	u32 magic;
	int err;

	err = request_firmware(&fw, setfile->path, isp->dev);
	if (err) {
		dev_err(isp->dev, "failed to request setfile '%s': %d\n",
			setfile->path, err);
		return err;
	}

	if (fw->size < setfile->size) {
		dev_err(isp->dev, "setfile too small (0x%lx/0x%zx)\n", fw->size,
			setfile->size);
		release_firmware(fw);
		return -EINVAL;
	}

	magic = be32_to_cpup((__be32 *)fw->data);
	if (magic != setfile->magic) {
		dev_err(isp->dev, "setfile '%s' corrupted?\n", setfile->path);
		release_firmware(fw);
		return -EINVAL;
	}

	isp_iowrite(isp, isp->data_surf->iova, (void *)fw->data, setfile->size);
	release_firmware(fw);

	return isp_cmd_ch_set_file_load(isp, ch, isp->data_surf->iova,
					setfile->size);
}

static int isp_ch_configure_capture(struct apple_isp *isp, u32 ch)
{
	struct isp_format *fmt = isp_get_format(isp, ch);
	int err;

	/* The setfile isn't requisite but then we don't get calibration */
	err = isp_ch_load_setfile(isp, ch);
	if (err) {
		dev_err(isp->dev, "warning: calibration data not loaded: %d\n",
			err);
	}

	err = isp_cmd_ch_sbs_enable(isp, ch, 1);
	if (err)
		return err;

	err = isp_cmd_ch_buffer_recycle_mode_set(
		isp, ch, CISP_BUFFER_RECYCLE_MODE_EMPTY_ONLY);
	if (err)
		return err;

	err = isp_cmd_ch_buffer_recycle_start(isp, ch);
	if (err)
		return err;

	err = isp_cmd_ch_camera_config_select(isp, ch, fmt->preset);
	if (err)
		return err;

	err = isp_cmd_ch_crop_set(isp, ch, fmt->x1, fmt->y1, fmt->x2, fmt->y2);
	if (err)
		return err;

	err = isp_cmd_ch_output_config_set(isp, ch, fmt->width, fmt->height,
					   CISP_COLORSPACE_REC709,
					   CISP_OUTPUT_FORMAT_NV12);
	if (err)
		return err;

	err = isp_cmd_ch_preview_stream_set(isp, ch, 1);
	if (err)
		return err;

	err = isp_cmd_ch_cnr_start(isp, ch);
	if (err)
		return err;

	err = isp_cmd_ch_mbnr_enable(isp, ch, 0, 1, 1);
	if (err)
		return err;

	err = isp_cmd_apple_ch_temporal_filter_start(isp, ch);
	if (err)
		return err;

	err = isp_cmd_apple_ch_motion_history_start(isp, ch);
	if (err)
		return err;

	err = isp_cmd_apple_ch_temporal_filter_enable(isp, ch);
	if (err)
		return err;

	err = isp_cmd_apple_ch_ae_fd_scene_metering_config_set(isp, ch);
	if (err)
		return err;

	err = isp_cmd_apple_ch_ae_metering_mode_set(isp, ch, 3);
	if (err)
		return err;

	err = isp_cmd_ch_ae_stability_set(isp, ch, 32);
	if (err)
		return err;

	err = isp_cmd_ch_ae_stability_to_stable_set(isp, ch, 20);
	if (err)
		return err;

	err = isp_cmd_ch_sif_pixel_format_set(isp, ch);
	if (err)
		return err;

	err = isp_cmd_ch_ae_frame_rate_max_set(isp, ch, ISP_FRAME_RATE_DEN);
	if (err)
		return err;

	err = isp_cmd_ch_ae_frame_rate_min_set(isp, ch, ISP_FRAME_RATE_DEN);
	if (err)
		return err;

	err = isp_cmd_ch_buffer_pool_config_set(isp, ch, CISP_POOL_TYPE_META);
	if (err)
		return err;

	err = isp_cmd_ch_buffer_pool_config_set(isp, ch,
						CISP_POOL_TYPE_META_CAPTURE);
	if (err)
		return err;

	return 0;
}

static int isp_configure_capture(struct apple_isp *isp)
{
	return isp_ch_configure_capture(isp, isp->current_ch);
}

int apple_isp_start_camera(struct apple_isp *isp)
{
	int err;

	err = apple_isp_firmware_boot(isp);
	if (err < 0) {
		dev_err(isp->dev, "failed to boot firmware: %d\n", err);
		return err;
	}

	err = isp_configure_capture(isp);
	if (err) {
		dev_err(isp->dev, "failed to configure capture: %d\n", err);
		apple_isp_firmware_shutdown(isp);
		return err;
	}

	return 0;
}

void apple_isp_stop_camera(struct apple_isp *isp)
{
	apple_isp_firmware_shutdown(isp);
}

int apple_isp_start_capture(struct apple_isp *isp)
{
	return isp_cmd_ch_start(isp, 0); // TODO channel mask
}

void apple_isp_stop_capture(struct apple_isp *isp)
{
	isp_cmd_ch_stop(isp, 0); // TODO channel mask
	isp_cmd_ch_buffer_return(isp, isp->current_ch);
}
