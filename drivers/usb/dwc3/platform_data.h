/**
 * platform_data.h - USB DWC3 Platform Data Support
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com
 * Author: Felipe Balbi <balbi@ti.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/usb/ch9.h>
#include <linux/usb/otg.h>

struct dwc3_platform_data {
	enum usb_device_speed maximum_speed;
	enum usb_dr_mode dr_mode;
	bool tx_fifo_resize;

	unsigned has_lpm_erratum:1;
	u32     quirks;

#define DWC3_QUIRK_AMD_NL		(1 << 0)
#define DWC3_QUIRK_DISSCRAMBLE		(1 << 1)
#define DWC3_QUIRK_U2EXIT_LFPS		(1 << 2)
#define DWC3_QUIRK_U2SSINP3OK		(1 << 3)
#define DWC3_QUIRK_REQP1P2P3		(1 << 4)
#define DWC3_QUIRK_DEPP1P2P3		(1 << 5)
#define DWC3_QUIRK_DEPOCHANGE		(1 << 6)
#define DWC3_QUIRK_LFPSFILT		(1 << 7)
#define DWC3_QUIRK_RX_DETOPOLL		(1 << 8)
#define DWC3_QUIRK_TX_DEEPH		(1 << 9)
#define DWC3_QUIRK_SUSPHY		(1 << 10)

};
