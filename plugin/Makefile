# EMU7800 PDK Plugin Makefile
# Cross-compile for webOS ARM device

# Toolchain - Linaro GCC for ARM
LINARO_PATH = $(HOME)/toolchains/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabi
CC = $(LINARO_PATH)/bin/arm-linux-gnueabi-gcc
STRIP = $(LINARO_PATH)/bin/arm-linux-gnueabi-strip

# Paths
# Use local PDL and SDL stub headers
INCLUDES = -Isrc -Isrc/include -Isrc/include/SDL

# Compiler flags
# -march=armv7-a for HP TouchPad (Cortex-A9)
# -MMD generates .d dependency files for headers
CFLAGS = -O2 -Wall -Wno-unused-variable -Wno-parentheses -march=armv7-a -MMD $(INCLUDES)

# Linker flags
# Link against stub libraries; real libs provided on device at runtime
LIBDIR = -Llib
LDFLAGS = $(LIBDIR) -Wl,-rpath-link,lib -Wl,--allow-shlib-undefined -l:libSDL-1.2.so.0 -l:libpdl.so -l:libGLES_CM.so -lm -lpthread -ldl

# Source files
SOURCES = \
	src/device.c \
	src/main.c \
	src/m6502.c \
	src/tia.c \
	src/tiasound.c \
	src/pia.c \
	src/cart.c \
	src/machine.c \
	src/video.c \
	src/audio.c \
	src/input.c \
	src/maria.c \
	src/pokeysound.c \
	src/font.c \
	src/filepicker.c \
	src/savestate.c \
	src/updater.c \
	src/sw_render.c

# Object files and dependency files
OBJECTS = $(SOURCES:.c=.o)
DEPS = $(SOURCES:.c=.d)

# Output
TARGET = emu7800.arm

# Default target
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Include auto-generated header dependencies
-include $(DEPS)

clean:
	rm -f $(OBJECTS) $(DEPS) $(TARGET)

# For local x86 testing (on host machine with SDL1.2)
# Note: Full host testing requires SDL1.2-dev installed
HOST_CC = gcc
HOST_INCLUDES = -Isrc -Isrc/include -Isrc/include/SDL
HOST_CFLAGS = -O2 -Wall -Wno-unused-variable -Wno-parentheses -g -DHOST_BUILD $(HOST_INCLUDES)
HOST_LDFLAGS = -Wl,--allow-shlib-undefined -lm

host: $(SOURCES)
	$(HOST_CC) $(HOST_CFLAGS) -o $(TARGET)_host $(SOURCES) $(HOST_LDFLAGS)

# Check syntax only (no linking required)
check:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only $(SOURCES)

# Pre3 diagnostic hello world (minimal: SDL + dlsym, no GL, no PDL link)
HELLO_SRC = src/hello_pre3.c
HELLO_TARGET = hello.arm
HELLO_LDFLAGS = $(LIBDIR) -Wl,-rpath-link,lib -Wl,--allow-shlib-undefined -l:libSDL-1.2.so.0 -lm -ldl

hello: $(HELLO_SRC)
	$(CC) $(CFLAGS) -o $(HELLO_TARGET) $(HELLO_SRC) $(HELLO_LDFLAGS)
	$(STRIP) $(HELLO_TARGET)

.PHONY: all clean host check hello
