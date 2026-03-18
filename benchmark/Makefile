CFLAGS := -O2 -Wall -Wextra -std=gnu11 -pthread
LDFLAGS := -lm

all: response_lat

response_lat: response_lat.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f response_lat

.PHONY: all clean
