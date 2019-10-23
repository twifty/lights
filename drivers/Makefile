# CONFIG_MODULE_SIG=n
MODULE_NAME = test

SRCS = \
	async.c \
	smbus/piix4.c \
	smbus/adapter.c

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)
OBJS = $(SRCS:.c=.o)

ifeq ($(KERNELRELEASE),)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) EXTRA_CFLAGS="-g -DDEBUG"

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: all clean

else

	obj-m += $(MODULE_NAME).o
	$(MODULE_NAME)-y = $(OBJS)

endif
