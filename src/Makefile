CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
LDFLAGS  ?=

all: reclocked

reclocked: reclocked.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f reclocked

.PHONY: all clean
