ADAPTERDIR = $(shell pwd)/adapter
export ADAPTERDIR

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
	if [[ ! -d build ]]; \
		then $(MAKE) build; \
	fi
	sudo insmod build/lights.ko;
	for module in $(MODULES); do \
		sudo insmod build/lights-$$module.ko; \
	done

# reinstall: uninstall build
# 	sudo insmod build/lights.ko;
# 	for module in $(MODULES); do \
# 		sudo insmod build/$$module.ko; \
# 	done

clean:
	$(MAKE) -C adapter clean;
	for dir in $(MODULES); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -rf build
