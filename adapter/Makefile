CONFIG_MODULE_SIG=n
MODULE_NAME = lights

SRCS = lights-adapter.c

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)
OBJS = $(SRCS:.c=.o)

ifeq ($(KERNELRELEASE),)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules EXTRA_CFLAGS="-g -DDEBUG"
	rm -rf *.o .*.o.d *~ core .depend .*.cmd *.mod *.mod.c .tmp_versions *.order

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: all clean

else

	obj-m += $(MODULE_NAME).o
	$(MODULE_NAME)-y = $(OBJS)

endif
