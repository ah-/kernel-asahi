// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Image Signal Processor driver
 *
 * Copyright (C) 2023 The Asahi Linux Contributors
 *
 * Based on aspeed/aspeed-video.c
 *  Copyright 2020 IBM Corp.
 *  Copyright (c) 2019-2020 Intel Corporation
 */

#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>

#include "isp-cam.h"
#include "isp-iommu.h"
#include "isp-v4l2.h"

static void apple_isp_detach_genpd(struct apple_isp *isp)
{
	if (isp->pd_count <= 1)
		return;

	for (int i = isp->pd_count - 1; i >= 0; i--) {
		if (isp->pd_link[i])
			device_link_del(isp->pd_link[i]);
		if (!IS_ERR_OR_NULL(isp->pd_dev[i]))
			dev_pm_domain_detach(isp->pd_dev[i], true);
	}

	return;
}

static int apple_isp_attach_genpd(struct apple_isp *isp)
{
	struct device *dev = isp->dev;

	isp->pd_count = of_count_phandle_with_args(
		dev->of_node, "power-domains", "#power-domain-cells");
	if (isp->pd_count <= 1)
		return 0;

	isp->pd_dev = devm_kcalloc(dev, isp->pd_count, sizeof(*isp->pd_dev),
				   GFP_KERNEL);
	if (!isp->pd_dev)
		return -ENOMEM;

	isp->pd_link = devm_kcalloc(dev, isp->pd_count, sizeof(*isp->pd_link),
				    GFP_KERNEL);
	if (!isp->pd_link)
		return -ENOMEM;

	for (int i = 0; i < isp->pd_count; i++) {
		isp->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR(isp->pd_dev[i])) {
			apple_isp_detach_genpd(isp);
			return PTR_ERR(isp->pd_dev[i]);
		}

		isp->pd_link[i] =
			device_link_add(dev, isp->pd_dev[i],
					DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
						DL_FLAG_RPM_ACTIVE);
		if (!isp->pd_link[i]) {
			apple_isp_detach_genpd(isp);
			return -EINVAL;
		}
	}

	return 0;
}

// TODO there has got to be a better way
static int apple_isp_resv_region(struct apple_isp *isp, int index)
{
	struct device *dev = isp->dev;
	int err;
	struct resource r;
	const __be32 *prop;
	struct isp_resv *resv = &isp->fw.resv[index];
	struct device_node *node =
		of_parse_phandle(dev->of_node, "memory-region", index);
	if (!node)
		return -EINVAL;

	if (of_address_to_resource(node, 0, &r)) {
		dev_err(dev, "failed to resolve memory-region address\n");
		of_node_put(node);
		return -EINVAL;
	}

	prop = of_get_property(node, "iommu-addresses", NULL);
	if (!prop) {
		dev_err(dev, "failed to read iommu-addresses\n");
		of_node_put(node);
		return -EINVAL;
	}
	prop++;

	resv->phys = r.start;
	resv->size = resource_size(&r);
	resv->iova = be64_to_cpup((const __be64 *)prop);
	of_node_put(node);

	isp_dbg(isp, "reserving: %d: phys: 0x%llx size: 0x%llx iova: 0x%llx\n",
		index, resv->phys, resv->size, resv->iova);

	err = iommu_map(isp->domain, resv->iova, resv->phys, resv->size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (err < 0)
		dev_err(dev, "failed to map reserved region\n");

	return err;
}

static void apple_isp_unresv_region(struct apple_isp *isp, int index)
{
	struct isp_resv *resv = &isp->fw.resv[index];
	iommu_unmap(isp->domain, resv->iova, resv->size);
	apple_isp_iommu_invalidate_tlb(isp);
}

static int apple_isp_init_iommu(struct apple_isp *isp)
{
	struct device *dev = isp->dev;
	struct isp_firmware *fw = &isp->fw;
	u64 heap_base, heap_size, vm_size;
	int err;
	int i = 0;

	isp->domain = iommu_get_domain_for_dev(isp->dev);
	if (!isp->domain)
		return -EPROBE_DEFER;
	isp->shift = __ffs(isp->domain->pgsize_bitmap);

	fw->count =
		of_count_phandle_with_args(dev->of_node, "memory-region", NULL);
	if ((fw->count <= 0) || (fw->count >= ISP_MAX_RESV_REGIONS)) {
		dev_err(dev, "invalid reserved region count (%d)\n", fw->count);
		return -EINVAL;
	}

	for (i = 0; i < fw->count; i++) {
		err = apple_isp_resv_region(isp, i);
		if (err < 0)
			goto out;
	}

	err = of_property_read_u64(dev->of_node, "apple,isp-heap-base",
				   &heap_base);
	if (err) {
		dev_err(dev, "failed to read 'apple,isp-heap-base': %d\n", err);
		goto out;
	}

	err = of_property_read_u64(dev->of_node, "apple,isp-heap-size",
				   &heap_size);
	if (err) {
		dev_err(dev, "failed to read 'apple,isp-heap-size': %d\n", err);
		goto out;
	}

	err = of_property_read_u64(dev->of_node, "apple,dart-vm-size",
				   &vm_size);
	if (err) {
		dev_err(dev, "failed to read 'apple,dart-vm-size': %d\n", err);
		goto out;
	}

	drm_mm_init(&isp->iovad, heap_base, vm_size - heap_base);

	/* Allocate read-only coprocessor private heap */
	fw->heap = isp_alloc_surface(isp, heap_size);
	if (!fw->heap) {
		drm_mm_takedown(&isp->iovad);
		err = -ENOMEM;
		goto out;
	}

	apple_isp_iommu_sync_ttbr(isp);

	return 0;

out:
	while (i--)
		apple_isp_unresv_region(isp, i);
	return err;
}

static void apple_isp_free_iommu(struct apple_isp *isp)
{
	isp_free_surface(isp, isp->fw.heap);
	drm_mm_takedown(&isp->iovad);
	for (int i = isp->fw.count; i-- > 0; )
		apple_isp_unresv_region(isp, i);
}

static int apple_isp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_isp *isp;
	struct resource *res;
	int err;

	isp = devm_kzalloc(dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	isp->dev = dev;
	isp->hw = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, isp);
	dev_set_drvdata(dev, isp);

	err = apple_isp_attach_genpd(isp);
	if (err) {
		dev_err(dev, "failed to attatch power domains\n");
		return err;
	}

	isp->asc = devm_platform_ioremap_resource_byname(pdev, "asc");
	if (IS_ERR(isp->asc)) {
		err = PTR_ERR(isp->asc);
		goto detach_genpd;
	}

	isp->mbox = devm_platform_ioremap_resource_byname(pdev, "mbox");
	if (IS_ERR(isp->mbox)) {
		err = PTR_ERR(isp->mbox);
		goto detach_genpd;
	}

	isp->gpio = devm_platform_ioremap_resource_byname(pdev, "gpio");
	if (IS_ERR(isp->gpio)) {
		err = PTR_ERR(isp->gpio);
		goto detach_genpd;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dart0");
	if (!res) {
		err = -ENODEV;
		goto detach_genpd;
	}

	/* Simply ioremap since it's a shared register zone */
	isp->dart0 = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(isp->dart0)) {
		err = PTR_ERR(isp->dart0);
		goto detach_genpd;
	}

	isp->dart1 = devm_platform_ioremap_resource_byname(pdev, "dart1");
	if (IS_ERR(isp->dart1)) {
		err = PTR_ERR(isp->dart1);
		goto detach_genpd;
	}

	isp->dart2 = devm_platform_ioremap_resource_byname(pdev, "dart2");
	if (IS_ERR(isp->dart2)) {
		err = PTR_ERR(isp->dart2);
		goto detach_genpd;
	}

	isp->irq = platform_get_irq(pdev, 0);
	if (isp->irq < 0) {
		err = isp->irq;
		goto detach_genpd;
	}
	if (!isp->irq) {
		err = -ENODEV;
		goto detach_genpd;
	}

	mutex_init(&isp->iovad_lock);
	mutex_init(&isp->video_lock);
	spin_lock_init(&isp->buf_lock);
	init_waitqueue_head(&isp->wait);
	INIT_LIST_HEAD(&isp->gc);
	INIT_LIST_HEAD(&isp->buffers);
	isp->wq = alloc_workqueue("apple-isp-wq", WQ_UNBOUND, 0);
	if (!isp->wq) {
		dev_err(dev, "failed to create workqueue\n");
		err = -ENOMEM;
		goto detach_genpd;
	}

	err = apple_isp_init_iommu(isp);
	if (err) {
		dev_err(dev, "failed to init iommu: %d\n", err);
		goto destroy_wq;
	}

	pm_runtime_enable(dev);

	err = apple_isp_detect_camera(isp);
	if (err) {
		dev_err(dev, "failed to detect camera: %d\n", err);
		goto free_iommu;
	}

	err = apple_isp_setup_video(isp);
	if (err) {
		dev_err(dev, "failed to register video device: %d\n", err);
		goto free_iommu;
	}

	dev_info(dev, "apple-isp probe!\n");

	return 0;

free_iommu:
	pm_runtime_disable(dev);
	apple_isp_free_iommu(isp);
destroy_wq:
	destroy_workqueue(isp->wq);
detach_genpd:
	apple_isp_detach_genpd(isp);
	return err;
}

static int apple_isp_remove(struct platform_device *pdev)
{
	struct apple_isp *isp = platform_get_drvdata(pdev);

	apple_isp_remove_video(isp);
	pm_runtime_disable(isp->dev);
	apple_isp_free_iommu(isp);
	destroy_workqueue(isp->wq);
	apple_isp_detach_genpd(isp);
	return 0;
}

/* T8020/T6000 registers */
#define DART_T8020_STREAM_COMMAND	     0x20
#define DART_T8020_STREAM_SELECT	     0x34
#define DART_T8020_TTBR			     0x200
#define DART_T8020_STREAM_COMMAND_INVALIDATE BIT(20)

static const struct apple_isp_hw apple_isp_hw_t8103 = {
	.pmu_base = 0x23b704000,

	.dsid_clr_base0 = 0x200014000,
	.dsid_clr_base1 = 0x200054000,
	.dsid_clr_base2 = 0x200094000,
	.dsid_clr_base3 = 0x2000d4000,
	.dsid_clr_range0 = 0x1000,
	.dsid_clr_range1 = 0x1000,
	.dsid_clr_range2 = 0x1000,
	.dsid_clr_range3 = 0x1000,

	.clock_scratch = 0x23b738010,
	.clock_base = 0x23bc3c000,
	.clock_bit = 0x1,
	.clock_size = 0x4,
	.bandwidth_scratch = 0x23b73800c,
	.bandwidth_base = 0x23bc3c000,
	.bandwidth_bit = 0x0,
	.bandwidth_size = 0x4,

	.stream_command = DART_T8020_STREAM_COMMAND,
	.stream_select = DART_T8020_STREAM_SELECT,
	.ttbr = DART_T8020_TTBR,
	.stream_command_invalidate = DART_T8020_STREAM_COMMAND_INVALIDATE,
};

static const struct apple_isp_hw apple_isp_hw_t6000 = {
	.pmu_base = 0x28e584000,

	.dsid_clr_base0 = 0x200014000,
	.dsid_clr_base1 = 0x200054000,
	.dsid_clr_base2 = 0x200094000,
	.dsid_clr_base3 = 0x2000d4000,
	.dsid_clr_range0 = 0x1000,
	.dsid_clr_range1 = 0x1000,
	.dsid_clr_range2 = 0x1000,
	.dsid_clr_range3 = 0x1000,

	.clock_scratch = 0x28e3d0868,
	.clock_base = 0x0,
	.clock_bit = 0x0,
	.clock_size = 0x8,
	.bandwidth_scratch = 0x28e3d0980,
	.bandwidth_base = 0x0,
	.bandwidth_bit = 0x0,
	.bandwidth_size = 0x8,

	.stream_command = DART_T8020_STREAM_COMMAND,
	.stream_select = DART_T8020_STREAM_SELECT,
	.ttbr = DART_T8020_TTBR,
	.stream_command_invalidate = DART_T8020_STREAM_COMMAND_INVALIDATE,
};

static const struct apple_isp_hw apple_isp_hw_t8110 = {
	.pmu_base = 0x23b704000,

	.dsid_clr_base0 = 0x200014000, // TODO
	.dsid_clr_base1 = 0x200054000,
	.dsid_clr_base2 = 0x200094000,
	.dsid_clr_base3 = 0x2000d4000,
	.dsid_clr_range0 = 0x1000,
	.dsid_clr_range1 = 0x1000,
	.dsid_clr_range2 = 0x1000,
	.dsid_clr_range3 = 0x1000,

	.clock_scratch = 0x23b3d0560,
	.clock_base = 0x0,
	.clock_bit = 0x0,
	.clock_size = 0x8,
	.bandwidth_scratch = 0x23b3d05d0,
	.bandwidth_base = 0x0,
	.bandwidth_bit = 0x0,
	.bandwidth_size = 0x8,

	.stream_command = DART_T8020_STREAM_COMMAND, // TODO
	.stream_select = DART_T8020_STREAM_SELECT,
	.ttbr = DART_T8020_TTBR,
	.stream_command_invalidate = DART_T8020_STREAM_COMMAND_INVALIDATE,
};

static const struct of_device_id apple_isp_of_match[] = {
	{ .compatible = "apple,t8103-isp", .data = &apple_isp_hw_t8103 },
	{ .compatible = "apple,t6000-isp", .data = &apple_isp_hw_t6000 },
	{},
};
MODULE_DEVICE_TABLE(of, apple_isp_of_match);

static __maybe_unused int apple_isp_suspend(struct device *dev)
{
	struct apple_isp *isp = dev_get_drvdata(dev);

	apple_isp_iommu_invalidate_tlb(isp);

	return 0;
}

static __maybe_unused int apple_isp_resume(struct device *dev)
{
	struct apple_isp *isp = dev_get_drvdata(dev);

	apple_isp_iommu_sync_ttbr(isp);

	return 0;
}
DEFINE_RUNTIME_DEV_PM_OPS(apple_isp_pm_ops, apple_isp_suspend, apple_isp_resume, NULL);

static struct platform_driver apple_isp_driver = {
	.driver	= {
		.name		= "apple-isp",
		.of_match_table	= apple_isp_of_match,
		.pm		= pm_ptr(&apple_isp_pm_ops),
	},
	.probe	= apple_isp_probe,
	.remove	= apple_isp_remove,
};
module_platform_driver(apple_isp_driver);

MODULE_AUTHOR("Eileen Yoon <eyn@gmx.com>");
MODULE_DESCRIPTION("Apple ISP driver");
MODULE_LICENSE("GPL v2");
