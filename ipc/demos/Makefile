CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?= -pthread

BINS = 01-epoll-lt-vs-et 02-pipe-wakeup 03-eventfd-cross-thread

.PHONY: all clean

all: $(BINS)

01-epoll-lt-vs-et: 01-epoll-lt-vs-et.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

02-pipe-wakeup: 02-pipe-wakeup.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

03-eventfd-cross-thread: 03-eventfd-cross-thread.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINS)
