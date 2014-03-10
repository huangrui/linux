#make CFLAGS+=-g -j4
sudo rmmod xhci_hcd
sudo rmmod dwc3
sudo rmmod dwc3_pci
sudo dmesg -C
sudo cp /home/amd/linux-usb/drivers/usb/host/xhci-hcd.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/host/
sudo cp /home/amd/linux-usb/drivers/usb/dwc3/dwc3.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/dwc3/
sudo cp /home/amd/linux-usb/drivers/usb/dwc3/dwc3-pci.ko /lib/modules/3.10.20-dwc3-pci+/kernel/drivers/usb/dwc3/
sudo sync
sleep 3
sudo modprobe dwc3_pci
