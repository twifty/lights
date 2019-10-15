ADAPTERDIR = $(shell pwd)/adapter
export ADAPTERDIR
MODULES = \
	aura

.PHONY: build clean adapter install uninstall $(MODULES)

adapter:
	mkdir -p build
	$(MAKE) -C adapter all
	mv adapter/lights.ko build/lights.ko

$(MODULES): adapter
	$(MAKE) -C $@ all
	mv $@/$@.ko build/$@.ko

.DEFAULT_GOAL :=
build: adapter $(MODULES)

uninstall:
	for module in $(MODULES); do \
		sudo rmmod $$module.ko || true; \
	done
	sudo rmmod build/lights.ko || true;

install: uninstall
	if [[ ! -d build ]]; \
		then $(MAKE) build; \
	fi
	sudo insmod build/lights.ko;
	for module in $(MODULES); do \
		sudo insmod build/$$module.ko; \
	done

clean:
	$(MAKE) -C adapter clean;
	for dir in $(MODULES); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -rf build
