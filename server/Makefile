
TARGET = aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

# Compiler settings
CXX ?= $(CROSS_COMPILE)c
CXXFLAGS ?= -Wall -Wextra -Werror 

# Output binary
OUTDIR = .
BINDIR = $(OUTDIR)
BIN = $(BINDIR)/$(TARGET)

.PHONY: all clean default

# Build on 'make' or 'make all' or 'make default'
all: $(BIN)
default: all

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.c
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

