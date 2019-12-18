ADAPTERDIR = $(shell pwd)/adapter
KERNEL ?= $(shell uname -r)
_KERNELDIRS = $(wildcard /lib/modules/${KERNEL}*/build)
KERNELDIR = $(shell echo $(_KERNELDIRS) | cut -d' ' -f1)

export ADAPTERDIR
export KERNELDIR

MODULES = \
	aura

.PHONY: build clean adapter install uninstall $(MODULES)

adapter:
	mkdir -p build
	$(MAKE) -C adapter all
	cp adapter/lights.ko build/lights.ko

$(MODULES): adapter
	$(MAKE) -C $@ all
	cp $@/lights-$@.ko build/lights-$@.ko

.DEFAULT_GOAL :=
build: adapter $(MODULES)

uninstall:
	for module in $(MODULES); do \
		sudo rmmod lights-$$module.ko || true; \
	done
	sudo rmmod lights.ko || true;

install: uninstall build
	sync
	if [[ ! -d build ]]; \
		then $(MAKE) build; \
	fi
	sudo insmod build/lights.ko;
	for module in $(MODULES); do \
		sudo insmod build/lights-$$module.ko; \
	done

clean:
	$(MAKE) -C adapter clean;
	for dir in $(MODULES); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -rf build
