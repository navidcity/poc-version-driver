obj-m := poc-version-driver.o

MY_CFLAGS += -g -DDEBUG
ccflags-y += ${MY_CFLAGS}

SRC := $(shell pwd)

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c
	rm -f Module.markers Module.symvers modules.order
	rm -rf .tmp_versions Modules.symvers
