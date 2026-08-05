CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all preview clean

all: v4l2_capture

preview: v4l2_capture_x11

v4l2_capture: v4l2_capture.c
	$(CC) $(CFLAGS) $< -o $@

v4l2_capture_x11: v4l2_capture.c
	$(CC) $(CFLAGS) -DHAVE_X11 $< -o $@ -lX11

clean:
	$(RM) v4l2_capture v4l2_capture_x11
