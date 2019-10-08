KBUILD_EXTRA_SYMBOLS := /home/owen/Work/kernel/lights/Module.symvers
CONFIG_MODULE_SIG=n
CONFIG_STACK_VALIDATION=n
MODULE_NAME = aura

SRCS = aura-module.c \
	aura-controller.c \
	memory/aura-memory.c \
	motherboard/aura-smbus-piix4.c \
	motherboard/aura-smbus-nuvoton.c \
	motherboard/aura-motherboard.c \
	gpu/asic/asic-polaris.c \
	gpu/asic/asic-vega.c \
	gpu/asic/asic-navi.c \
	gpu/aura-gpu-reg.c \
	gpu/aura-gpu-i2c.c \
	gpu/aura-gpu.c \
	header/usb-transfer.c \
	header/usb-driver.c
	# header/aura-header.c

KERNELDIR = /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)
OBJS = $(SRCS:.c=.o)

ifeq ($(KERNELRELEASE),)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules EXTRA_CFLAGS="-g -DDEBUG"
	rm -rf *.o *~ core .depend .*.cmd *.mod.c .tmp_versions

parent:
	sudo rmmod $(MODULE_NAME).ko || true
	$(MAKE) -C ../lights install

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install: clean all
	sudo insmod ../lights/lights.ko || true
	sudo rmmod $(MODULE_NAME).ko || true
	sudo insmod $(MODULE_NAME).ko
	rm -rf *.o *~ core .depend .*.cmd *.mod.c .tmp_versions

.PHONY: all clean

else

	obj-m += $(MODULE_NAME).o
	$(MODULE_NAME)-y = $(OBJS)

endif
