/*
 * AMD ACPI support for ACPI2platform device.
 *
 * Copyright (c) 2014,2015 AMD Corporation.
 * Authors: Ken Xue <Ken.Xue@amd.com>
 *          Jeff Wu <15618388108@163.com>
 *          Wang Hongcheng <Annie.Wang@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/clkdev.h>
#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/pm.h>
#include <linux/amba/bus.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/amba/pl330.h>
#include <linux/platform_data/8250-dw.h>

#include "internal.h"

ACPI_MODULE_NAME("acpi_apd");
struct apd_private_data;

/**
 * ACPI_APD_SYSFS : add device attributes in sysfs
 * ACPI_APD_PM : attach power domain to device
 */
#define ACPI_APD_SYSFS	BIT(0)
#define ACPI_APD_PM	BIT(1)

static u8 peri_id[2] = { 0, 1 };

static struct dma_pl330_platdata amd_pl330[] = {
	{
		.nr_valid_peri = 2,
		.peri_id = peri_id,
		.mcbuf_sz = 0,
		.flags = IRQF_SHARED,
		.base_request_line = 1,
		.num = 0,
	},
	{
		.nr_valid_peri = 2,
		.peri_id = peri_id,
		.mcbuf_sz = 0,
		.flags = IRQF_SHARED,
		.base_request_line = 2,
		.num = 0,
	}
};

static struct plat_dw8250_data amd_dw8250 = {
	.has_pl330_dma = 1,
};

/**
 * struct apd_device_desc - a descriptor for apd device
 * @flags: device flags like %ACPI_APD_SYSFS, %ACPI_APD_PM
 * @fixed_clk_rate: fixed rate input clock source for acpi device;
 *			0 means no fixed rate input clock source
 * @setup: a hook routine to set device resource during create platform device
 * @post_setup: an additional hook routine
 * Device description defined as acpi_device_id.driver_data
 */
struct apd_device_desc {
	unsigned int flags;
	unsigned int fixed_clk_rate;
	unsigned int base_offset;
	unsigned int periphid;
	int (*setup)(struct apd_private_data *pdata);
	int (*post_setup)(struct apd_private_data *pdata);
};

struct apd_private_data {
	struct clk *clk;
	struct acpi_device *adev;
	const struct apd_device_desc *dev_desc;
	struct platform_device *pdev;
};

#ifdef CONFIG_X86_AMD_PLATFORM_DEVICE
#define APD_ADDR(desc)	((unsigned long)&desc)

static int acpi_apd_setup(struct apd_private_data *pdata)
{
	const struct apd_device_desc *dev_desc = pdata->dev_desc;
	struct clk *clk = ERR_PTR(-ENODEV);

	if (dev_desc->fixed_clk_rate) {
		clk = clk_register_fixed_rate(&pdata->adev->dev,
					dev_name(&pdata->adev->dev),
					NULL, CLK_IS_ROOT,
					dev_desc->fixed_clk_rate);
		clk_register_clkdev(clk, NULL, dev_name(&pdata->adev->dev));
		pdata->clk = clk;
	}

	return 0;
}

#ifdef CONFIG_SERIAL_8250_AMD
static int acpi_apd_setup_quirks(struct apd_private_data *pdata)
{
	struct amba_device *amba_dev = NULL;
	struct platform_device *pdev = pdata->pdev;
	struct resource *presource, *resource = NULL;
	int count = 0;
	int ret = 0;
	unsigned int i;
	unsigned int irq[AMBA_NR_IRQS];
	struct clk *clk = ERR_PTR(-ENODEV);
	char amba_devname[100];
	int devnum;

	ret = platform_device_add_data(pdev, &amd_dw8250,
				       sizeof(amd_dw8250));
	if (ret)
		goto resource_alloc_err;

	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (!resource)
		goto resource_alloc_err;

	presource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	resource->parent = presource;

	/*
	 * The memory address of AMD pl330 has an offset of ACPI
	 * mem resource.
	 */
	resource->start += presource->start + pdata->dev_desc->base_offset;
	resource->end = presource->end;

	presource = pdev->resource;
	for (i = 0; i < resource_size(presource); i++) {
		if (presource[i].flags & IORESOURCE_IRQ)
			irq[count++] = presource[i].start;
	}

	sprintf(amba_devname, "%s%s", dev_name(&pdev->dev), "DMA");

	amba_dev = amba_device_alloc(amba_devname,
				     resource->start,
				     resource_size(resource));

	if (!amba_dev)
		goto amba_alloc_err;

	devnum = amba_devname[strlen(dev_name(&pdev->dev)) - 1] - '0';

	amba_dev->dev.coherent_dma_mask
		= acpi_dma_supported(ACPI_COMPANION(&pdev->dev)) ? DMA_BIT_MASK(64) : 0;
	amba_dev->dev.platform_data = &amd_pl330[devnum];
	amba_dev->dev.fwnode = acpi_fwnode_handle(ACPI_COMPANION(&pdev->dev));

	amba_dev->dev.parent = &pdev->dev;
	amba_dev->periphid = pdata->dev_desc->periphid;

	WARN_ON_ONCE(count > AMBA_NR_IRQS);

	for (i = 0; i < count; i++)
		amba_dev->irq[i] = irq[i];

	clk = clk_register_fixed_rate(&amba_dev->dev,
				      dev_name(&amba_dev->dev),
				      NULL, CLK_IS_ROOT,
				      pdata->dev_desc->fixed_clk_rate);
	if (IS_ERR_OR_NULL(clk))
		goto amba_register_err;

	ret = clk_register_clkdev(clk, "apb_pclk",
				  dev_name(&amba_dev->dev));
	if (ret)
		goto amba_register_err;

	amba_dev->dev.init_name = NULL;
	ret = amba_device_add(amba_dev, resource);
	if (ret)
		goto amba_register_err;

	kfree(resource);

	dma_cap_set(DMA_MEMCPY, amd_pl330[devnum].cap_mask);
	dma_cap_set(DMA_SLAVE, amd_pl330[devnum].cap_mask);
	dma_cap_set(DMA_CYCLIC, amd_pl330[devnum].cap_mask);
	dma_cap_set(DMA_PRIVATE, amd_pl330[devnum].cap_mask);

	return 0;

amba_register_err:
	amba_device_put(amba_dev);

amba_alloc_err:
	kfree(resource);

resource_alloc_err:
	dev_info(&pdev->dev, "AMBA device created failed.\n");
	return 0;
}

#else

static int acpi_apd_setup_quirks(struct apd_private_data *pdata)
{
	return 0;
}

#endif

static struct apd_device_desc cz_i2c_desc = {
	.setup = acpi_apd_setup,
	.fixed_clk_rate = 133000000,
};

static struct apd_device_desc cz_uart_desc = {
	.setup = acpi_apd_setup,
	.post_setup = acpi_apd_setup_quirks,
	.fixed_clk_rate = 48000000,
	.periphid = 0x00041330,
	.base_offset = SZ_4K,
};

#else

#define APD_ADDR(desc) (0UL)

#endif /* CONFIG_X86_AMD_PLATFORM_DEVICE */

/**
 * Create platform device during acpi scan attach handle.
 * Return value > 0 on success of creating device.
 */
static int acpi_apd_create_device(struct acpi_device *adev,
				  const struct acpi_device_id *id)
{
	const struct apd_device_desc *dev_desc = (void *)id->driver_data;
	struct apd_private_data *pdata;
	struct platform_device *pdev;
	int ret;

	if (!dev_desc) {
		pdev = acpi_create_platform_device(adev);
		return IS_ERR_OR_NULL(pdev) ? PTR_ERR(pdev) : 1;
	}

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->adev = adev;
	pdata->dev_desc = dev_desc;

	if (dev_desc->setup) {
		ret = dev_desc->setup(pdata);
		if (ret)
			goto err_out;
	}

	adev->driver_data = pdata;

	pdev = acpi_create_platform_device(adev);
	ret = PTR_ERR(pdev);
	if (IS_ERR_OR_NULL(pdev))
		goto err_out;

	if (dev_desc->post_setup) {
		pdata->pdev = pdev;
		dev_desc->post_setup(pdata);
	}

	return ret;

 err_out:
	adev->driver_data = NULL;
	kfree(pdata);
	return ret;
}

static const struct acpi_device_id acpi_apd_device_ids[] = {
	/* Generic apd devices */
	{ "AMD0010", APD_ADDR(cz_i2c_desc) },
	{ "AMD0020", APD_ADDR(cz_uart_desc) },
	{ "AMD0030", },
	{ }
};

static struct acpi_scan_handler apd_handler = {
	.ids = acpi_apd_device_ids,
	.attach = acpi_apd_create_device,
};

void __init acpi_apd_init(void)
{
	acpi_scan_add_handler(&apd_handler);
}
