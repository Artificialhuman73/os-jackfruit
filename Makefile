KERNEL_SRC := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Compiler flags
CFLAGS := -Wall -Wextra -O2 -g
LDFLAGS := -pthread

# Targets
BINARIES := engine cpu_hog io_pulse memory_hog
KERNEL_MOD := monitor.ko

# Default target
all: $(BINARIES) $(KERNEL_MOD)

# User-space binaries
engine: engine.c monitor_ioctl.h
	gcc $(CFLAGS) $(LDFLAGS) -o $@ $<

cpu_hog: cpu_hog.c
	gcc $(CFLAGS) -o $@ $<

io_pulse: io_pulse.c
	gcc $(CFLAGS) -o $@ $<

memory_hog: memory_hog.c
	gcc $(CFLAGS) -o $@ $<

# Kernel module
$(KERNEL_MOD): monitor.c monitor_ioctl.h
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

# CI target: build only user-space binaries (no kernel module)
.PHONY: ci
ci: engine cpu_hog io_pulse memory_hog
	@echo "CI build successful"
	@./engine 2>&1 | head -1 || true

# Clean
.PHONY: clean
clean:
	rm -f $(BINARIES)
	rm -f *.o *.ko *.mod.c *.mod.o .*.cmd
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

# Install (copy to PATH)
.PHONY: install
install: all
	sudo cp engine /usr/local/bin/
	sudo cp cpu_hog /usr/local/bin/
	sudo cp io_pulse /usr/local/bin/
	sudo cp memory_hog /usr/local/bin/

# Load kernel module
.PHONY: insmod
insmod: $(KERNEL_MOD)
	sudo insmod ./monitor.ko

# Unload kernel module
.PHONY: rmmod
rmmod:
	sudo rmmod monitor 2>/dev/null || true

.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all      - Build everything (binaries and kernel module)"
	@echo "  ci       - Build only user-space binaries (GitHub Actions compatible)"
	@echo "  clean    - Remove build artifacts"
	@echo "  insmod   - Load kernel module"
	@echo "  rmmod    - Unload kernel module"
	@echo "  install  - Install binaries to /usr/local/bin"
