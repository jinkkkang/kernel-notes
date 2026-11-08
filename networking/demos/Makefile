CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

BINS = 01-sock-stats 02-unix-scm-fd

.PHONY: all clean run

all: $(BINS)

01-sock-stats: 01-sock-stats.c
	$(CC) $(CFLAGS) -o $@ $<

02-unix-scm-fd: 02-unix-scm-fd.c
	$(CC) $(CFLAGS) -o $@ $<

run: all
	./01-sock-stats
	./02-unix-scm-fd
	sudo -n ./03-veth-ping.sh

clean:
	rm -f $(BINS)
