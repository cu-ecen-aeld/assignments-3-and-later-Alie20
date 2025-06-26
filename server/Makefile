
TARGET = aesdsocket
SRCS = aesdsocket.cpp
OBJS = $(SRCS:.cpp=.o)

# Compiler settings
CXX ?= g++
CXXFLAGS ?= -Wall -Wextra -Werror -std=c++11

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

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

