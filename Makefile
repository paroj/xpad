obj-m = xpad.o

KVERSION = $(shell uname -r)
all:
	make -C /lib/modules/$(KVERSION)/build V=1 M=$(PWD) modules
clean:
	test ! -d /lib/modules/$(KVERSION) || make -C /lib/modules/$(KVERSION)/build V=1 M=$(PWD) clean

## Allow Install on Gentoo to work [huge thanks to hurikhan77 for figuring this part out]:
## Note: you need to make the xpad built in driver set to module in kernel config for this to work
## Once installed, run modprobe xpad to start it and it should work
KERNEL_SOURCE_DIR := /lib/modules/$(shell uname -r)/source
LD := ld.bfd

modules_install:
	$(MAKE) -C $(KERNEL_SOURCE_DIR) INSTALL_MOD_DIR="kernel/drivers/input/joystick" LD=$(LD) M=$(shell pwd) $@
