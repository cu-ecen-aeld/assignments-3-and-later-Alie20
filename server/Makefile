TARGET = aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

# Compiler settings
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -Werror

# --------------------------------------------------
# 🔧 Assignment 8 build option
# --------------------------------------------------
# Default: use /dev/aesdchar for reads/writes
USE_AESD_CHAR_DEVICE ?= 1

# Pass the macro to compiler
CFLAGS += -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)
# --------------------------------------------------

# Output binary
OUTDIR = .
BINDIR = $(OUTDIR)
BIN = $(BINDIR)/$(TARGET)

.PHONY: all clean default

# Build on 'make' or 'make all' or 'make default'
all: $(BIN)
default: all

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

