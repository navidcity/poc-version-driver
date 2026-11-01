# obj-m := poc-version-driver.o

# MY_CFLAGS += -g -DDEBUG
# ccflags-y += ${MY_CFLAGS}

# SRC := $(shell pwd)

# modules:
# 	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

# modules_install:
# 	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

# bear:
# 	bear --append --output $(PWD)/.vscode/compile_commands.json -- $(MAKE) -C $(KDIR) M=$(PWD) modules

# clean:
# 	rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c
# 	rm -f Module.markers Module.symvers modules.order
# 	rm -rf .tmp_versions Modules.symvers





obj-m += poc-version-driver.o

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Default target: build the module
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

bear:
	bear --append --output $(PWD)/.vscode/compile_commands.json -- $(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean generated files
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install the module into the running kernel
install:
	sudo $(MAKE) -C $(KDIR) M=$(PWD) modules_install
	sudo depmod -a

# Load/unload helpers
load:
	sudo modprobe mymodule

unload:
	sudo modprobe -r mymodule

.PHONY: all bear clean install load unload