CONFIG_MODULE_SIG=n
MODULE_NAME = lights

SRCS = lights-class.c

KERNELDIR = /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)
OBJS = $(SRCS:.c=.o)

ifeq ($(KERNELRELEASE),)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules EXTRA_CFLAGS="-g -DDEBUG"
	rm -rf *.o *~ core .depend .*.cmd *.mod.c .tmp_versions *.order

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install: clean all
	sudo rmmod $(MODULE_NAME).ko || true
	sudo insmod $(MODULE_NAME).ko
	rm -rf *.o *~ core .depend .*.cmd *.mod.c .tmp_versions *.order

.PHONY: all clean

else

	obj-m += $(MODULE_NAME).o
	$(MODULE_NAME)-y = $(OBJS)

endif
