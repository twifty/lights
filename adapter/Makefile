CONFIG_MODULE_SIG=n
MODULE_NAME = lights

SRCS = \
	lights-interface.c \
	lights-adapter.c \
	lib/async.c \
	lib/reserve.c \
	usb/usb-driver.c \
	smbus/piix4.c \
	smbus/nuvoton.c

PWD = $(shell pwd)
OBJS = $(SRCS:.c=.o)

ifeq ($(KERNELRELEASE),)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules EXTRA_CFLAGS="-g -Wall -DDEBUG -I$(PWD)/../"

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: all clean

else

	obj-m += $(MODULE_NAME).o
	$(MODULE_NAME)-y = $(OBJS)

endif
