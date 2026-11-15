CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

.PHONY: all clean

all: 01-minimal-kvm

01-minimal-kvm: 01-minimal-kvm.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f 01-minimal-kvm
