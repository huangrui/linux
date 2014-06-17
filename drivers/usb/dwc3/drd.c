/**
 * drd.c - Dual Role Switch Funciton File
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Author: Huang Rui <ray.huang@amd.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include "core.h"
#include "gadget.h"
#include "io.h"
#include "drd.h"


int dwc3_drd_to_host(struct dwc3 *dwc)
{
	int ret;

	if (dwc->has_xhci)
		dwc3_host_exit(dwc);
	if (dwc->has_gadget)
		dwc3_gadget_stop_on_switch(dwc);

	dwc3_core_soft_reset(dwc);

	ret = dwc3_event_buffers_setup(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to setup event buffers\n");
		goto err0;
	}

	dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_HOST);

	ret = dwc3_host_init(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to init host\n");
		goto err0;
	}
err0:
	return ret;
}

int dwc3_drd_to_device(struct dwc3 *dwc)
{
	int ret;
	unsigned long timeout;
	u32 reg;

	if (dwc->has_xhci)
		dwc3_host_exit(dwc);
	dev_dbg(dwc->dev, "has_gadget=%d, in to_device\n", dwc->has_gadget);
	if (dwc->has_gadget)
		dwc3_gadget_stop_on_switch(dwc);

	dwc3_core_soft_reset(dwc);

	dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);

	/* issue device SoftReset too */
	timeout = jiffies + msecs_to_jiffies(500);
	dwc3_writel(dwc->regs, DWC3_DCTL, DWC3_DCTL_CSFTRST);
	do {
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		if (!(reg & DWC3_DCTL_CSFTRST))
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(dwc->dev, "Reset Timed Out\n");
			ret = -ETIMEDOUT;
			goto err0;
		}

		cpu_relax();
	} while (true);

	ret = dwc3_event_buffers_setup(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to setup event buffers\n");
		goto err0;
	}

	ret = dwc3_gadget_restart(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to init gadget\n");
		goto err0;
	}
err0:
	return ret;
}

