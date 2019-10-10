ADAPTERDIR = $(shell pwd)/adapter
export ADAPTERDIR
MODULES = \
	aura

.PHONY: all clean adapter install uninstall $(MODULES)

adapter:
	mkdir -p build
	$(MAKE) -C adapter all
	mv adapter/lights.ko build/lights.ko

$(MODULES): adapter
	$(MAKE) -C $@ all
	mv $@/$@.ko build/$@.ko

.DEFAULT_GOAL :=
all: adapter $(MODULES)

uninstall:
	for module in $(MODULES); do \
		sudo rmmod $$module.ko || true; \
	done
	sudo rmmod build/lights.ko || true;

install: all uninstall
	sudo insmod build/lights.ko;
	for module in $(MODULES); do \
		sudo insmod $$module.ko; \
	done

clean:
	$(MAKE) -C adapter clean;
	for dir in $(MODULES); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -rf build
