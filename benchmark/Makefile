CFLAGS := -O2 -Wall -Wextra -std=gnu11 -pthread
LDFLAGS := -lm

all: asymmetric_drain

asymmetric_drain: asymmetric_drain.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f asymmetric_drain

.PHONY: all clean
