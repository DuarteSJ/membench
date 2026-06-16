# membench build.
#
# NUMA pinning (-X/-A/-B <node>) is core to this tiered-memory suite, so libnuma
# is required. Install libnuma-dev / numactl if linking -lnuma fails.

CC      ?= gcc
CFLAGS  ?= -O3 -march=native -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing
LDFLAGS ?=
LDLIBS  := -lpthread -lnuma

OBJS := main.o workloads.o
BIN  := membench

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c membench.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: clean
