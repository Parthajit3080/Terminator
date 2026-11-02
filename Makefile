CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
THREAD_FLAGS ?= -pthread
X11_LIBS ?= -lX11

.PHONY: all clean

all: terminator

terminator: terminator.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -o $@ $< $(LDFLAGS) $(X11_LIBS) $(THREAD_FLAGS)

clean:
	rm -f terminator