# Compiler and flags
CC=g++
CXX=g++
RANLIB=ranlib
AR=ar
ARFLAGS=rcs

# Source files and object files
LIBSRC=MapReduceFramework.cpp Barrier.cpp
LIBOBJ=$(LIBSRC:.cpp=.o)
HEADERS=Barrier.h

# Include directories and flags
INCS=-I.
CFLAGS = -Wall -pthread -std=c++11 -g $(INCS)
CXXFLAGS = -Wall -pthread -std=c++11 -g $(INCS)

# Library and targets
OSMLIB = libMapReduceFramework.a
TARGETS = $(OSMLIB)

# Tar settings
TAR=tar
TARFLAGS=-cvf
TARNAME=ex3.tar
TARSRCS=$(LIBSRC) $(HEADERS) Makefile README

# Default target
all: $(TARGETS)

# Create the library
$(TARGETS): $(LIBOBJ)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

# Clean up build files
clean:
	$(RM) $(TARGETS) $(LIBOBJ) *~ *core

# Create a tar file
tar:
	$(TAR) $(TARFLAGS) $(TARNAME) $(TARSRCS)

# Dependency management
depend:
	makedepend -- $(CFLAGS) -- $(LIBSRC)

# Phony targets
.PHONY: all clean tar depend
