CONFIG_MODULE_SIG=n
CONFIG_STACK_VALIDATION=n
MODULE_NAME = lights-aura

SRCS = \
	aura-module.c \
	controller/aura-controller.c \
	motherboard/aura-motherboard.c \
	memory/aura-memory.c \
	gpu/asic/asic-polaris.c \
	gpu/asic/asic-vega.c \
	gpu/asic/asic-navi.c \
	gpu/aura-gpu-reg.c \
	gpu/aura-gpu-i2c.c \
	gpu/aura-gpu.c \
	header/aura-header.c

# KERNELDIR = /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)
KBUILD_EXTRA_SYMBOLS := $(ADAPTERDIR)/Module.symvers
OBJS = $(SRCS:.c=.o)

ifeq ($(KERNELRELEASE),)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules EXTRA_CFLAGS="-g -DDEBUG -I$(PWD)/../"

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: all clean

else

	obj-m += $(MODULE_NAME).o
	$(MODULE_NAME)-y = $(OBJS)

endif
