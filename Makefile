# External-module build for the IGS PCCARD driver.
#
# Linux 2.6 and later:
#   make KERNELDIR=/lib/modules/$(uname -r)/build
#   insmod ./pccard.ko
#
# Linux 2.4:
#   make linux24 KERNELDIR=/usr/src/linux-2.4
#   insmod ./pccard.o

ifneq ($(KERNELRELEASE),)

obj-m := pccard.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= gcc

LINUX24_CFLAGS ?= \
	-D__KERNEL__ \
	-DMODULE \
	-I$(KERNELDIR)/include \
	-Wall \
	-Wstrict-prototypes \
	-Wno-trigraphs \
	-O2 \
	-fno-strict-aliasing \
	-fno-common \
	-Wno-unused \
	-fomit-frame-pointer \
	-pipe \
	-mpreferred-stack-boundary=2 \
	-march=i686 \
	-nostdinc \
	-iwithprefix include

SRC := pccard.c

.PHONY: all linux24 clean insmod insmod24 rmmod

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

linux24: pccard.o

pccard.o: $(SRC) defs.h
	$(CC) $(LINUX24_CFLAGS) -DKBUILD_BASENAME=pccard -c -o $@ $(SRC)

insmod: all
	/sbin/insmod ./pccard.ko

insmod24: linux24
	/sbin/insmod ./pccard.o

rmmod:
	-/sbin/rmmod pccard

clean:
	if [ -d "$(KERNELDIR)" ]; then $(MAKE) -C $(KERNELDIR) M=$(PWD) clean; fi
	rm -f pccard.o *.ko *.mod.c .*.cmd *.o.cmd Module.symvers modules.order

endif
