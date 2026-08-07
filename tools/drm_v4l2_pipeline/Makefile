CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all clean

all: drm_v4l2_pipeline

drm_v4l2_pipeline: drm_v4l2_pipeline.c
	$(CC) $(CFLAGS) $< -o $@ $(shell pkg-config --cflags --libs libdrm)

clean:
	$(RM) drm_v4l2_pipeline
