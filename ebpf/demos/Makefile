CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

BINS = 04-map-user 03-xdp-testrun 01-kprobe-count

all: $(BINS)

04-map-user: 04-map-user.c
	$(CC) $(CFLAGS) -o $@ $<

03-xdp-testrun: 03-xdp-testrun.c bpf_insn.h
	$(CC) $(CFLAGS) -o $@ $<

01-kprobe-count: 01-kprobe-count.c bpf_insn.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
