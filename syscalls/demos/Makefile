CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread

BINS := 01-vdso-vs-syscall 02-bad-pointer-efault 03-restart-eintr

all: $(BINS)

01-vdso-vs-syscall: 01-vdso-vs-syscall.c
	$(CC) $(CFLAGS) -o $@ $<

02-bad-pointer-efault: 02-bad-pointer-efault.c
	$(CC) $(CFLAGS) -o $@ $<

03-restart-eintr: 03-restart-eintr.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
