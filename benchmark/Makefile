CFLAGS := -O2 -Wall -Wextra -std=gnu11 -pthread
LDFLAGS := -lm

all: selection_bench

selection_bench: selection_bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f selection_bench

.PHONY: all clean
