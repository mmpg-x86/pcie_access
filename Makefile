KDIR ?= /lib/modules/$(shell uname -r)/build

PROGRAM = pcie_access
obj-m = pcie_access_drv.o
pcie_access_drv-y := pcie_stub.o

CC ?= gcc
override CFLAGS += -Wall -Wextra -O2

.PHONY: all kbuild user clean deb

all: kbuild user

kbuild:
	$(MAKE) -C $(KDIR) M=$(PWD)

user:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROGRAM) pcie_access.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ./$(PROGRAM)

deb:
	dpkg-buildpackage -us -uc -b
	@echo "Packages built in parent directory:"
	@ls -1 ../pcie-stub-dkms_*.deb ../pcie-access_*.deb 2>/dev/null || true
