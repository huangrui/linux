#make CFLAGS+=-g -j4
sudo rmmod usb_f_ss_lb
sudo rmmod g_zero
sudo rmmod libcomposite
sudo rmmod dwc3
sudo rmmod udc_core
sudo rmmod dwc3_pci
sudo dmesg -C
sudo cp /home/amd/linux-usb/drivers/usb/gadget/g_zero.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/gadget/
sudo cp /home/amd/linux-usb/drivers/usb/gadget/libcomposite.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/gadget/
sudo cp /home/amd/linux-usb/drivers/usb/gadget/usb_f_ss_lb.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/gadget/
sudo cp /home/amd/linux-usb/drivers/usb/gadget/udc-core.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/gadget/
sudo cp /home/amd/linux-usb/drivers/usb/dwc3/dwc3.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/dwc3/
sudo cp /home/amd/linux-usb/drivers/usb/dwc3/dwc3-pci.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/dwc3/
sudo sync
sleep 3
sudo modprobe dwc3_pci
sudo modprobe g_zero
