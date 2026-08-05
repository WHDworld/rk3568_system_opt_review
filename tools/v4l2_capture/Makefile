CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all clean

all: v4l2_capture

v4l2_capture: v4l2_capture.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	$(RM) v4l2_capture
