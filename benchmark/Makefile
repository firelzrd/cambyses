CFLAGS ?= -O2 -Wall

fill: fill.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f fill

.PHONY: clean
