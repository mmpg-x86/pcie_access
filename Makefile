KDIR ?= /lib/modules/$(shell uname -r)/build

PROGRAM = pcie_access
obj-m = pcie_stub.o

CC ?= gcc
override CFLAGS += -Wall -Wextra -O2

.PHONY: all kbuild user clean

all: kbuild user

kbuild:
	$(MAKE) -C $(KDIR) M=$(PWD)

user:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROGRAM) pcie_access.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ./$(PROGRAM)
