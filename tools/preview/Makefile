# Host preview renderer for the eye engine.
#
#   make            build ./preview
#   make sheet      build, render, convert to PNG
#   make check      verify the dirty-rect flush converges (see mirror_test.c)
#   make clean

CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra

ROOT    := ../..
SRC     := $(ROOT)/src

# The stub include dir shadows display/sh1106.h with a version that only
# declares the panel size and the pixel sink, so eyes.c compiles without the
# Pico SDK.
INCS    := -Istub -I$(SRC)

preview: preview.c $(SRC)/eyes.c $(SRC)/display/font5x7.c
	$(CC) $(CFLAGS) $(INCS) preview.c $(SRC)/eyes.c $(SRC)/display/font5x7.c -o preview -lm

sheet: preview
	./preview preview.ppm
	ffmpeg -loglevel error -y -i preview.ppm preview.png
	@echo "wrote preview.png"

mirror_test: mirror_test.c $(SRC)/eyes.c
	$(CC) $(CFLAGS) $(INCS) mirror_test.c $(SRC)/eyes.c -o mirror_test -lm

banner_test: banner_test.c $(SRC)/eyes.c $(SRC)/display/font5x7.c
	$(CC) $(CFLAGS) $(INCS) -I$(SRC)/display banner_test.c $(SRC)/eyes.c $(SRC)/display/font5x7.c -o banner_test -lm

check: mirror_test banner_test
	./mirror_test
	./banner_test

clean:
	rm -f preview preview.ppm preview.png mirror_test banner_test

.PHONY: sheet check clean
