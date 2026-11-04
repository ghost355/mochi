# Host preview renderer for the eye engine.
#
#   make            build ./preview
#   make sheet      build, render, convert to PNG
#   make clean

CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra

ROOT    := ../..
SRC     := $(ROOT)/src

# The stub include dir shadows display/sh1106.h with a version that only
# declares the panel size and the pixel sink, so eyes.c compiles without the
# Pico SDK.
INCS    := -Istub -I$(SRC)

preview: preview.c $(SRC)/eyes.c
	$(CC) $(CFLAGS) $(INCS) preview.c $(SRC)/eyes.c -o preview -lm

sheet: preview
	./preview preview.ppm
	ffmpeg -loglevel error -y -i preview.ppm preview.png
	@echo "wrote preview.png"

clean:
	rm -f preview preview.ppm preview.png

.PHONY: sheet clean
