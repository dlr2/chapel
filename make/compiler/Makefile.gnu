#
# settings for gnu-compatible platforms
#

#
# Tools
#
CXX = g++
CC = gcc
MAKEDEPEND = $(CXX) -MM -MG
CMAKEDEPEND = $(CC) -MM -MG

RANLIB = ranlib


#
# General Flags
#

DEBUG_CFLAGS = -g
OPT_CFLAGS = -O3
PROFILE_CFLAGS = -pg
PROFILE_LFLAGS = -pg

ifdef CHPL_GCOV
CFLAGS += -fprofile-arcs -ftest-coverage
LDFLAGS += -fprofile-arcs
endif


#
# Flags for compiler, runtime, and generated code
#
COMP_CFLAGS = $(CFLAGS)
COMP_CFLAGS_NONCHPL = -Wno-error
RUNTIME_CFLAGS = -std=c99 $(CFLAGS)
RUNTIME_GEN_CFLAGS = $(RUNTIME_CFLAGS)
GEN_CFLAGS = -std=c99

ifeq ($(CHPL_MAKE_PLATFORM), darwin)
# build 64-bit binaries when on a 64-bit capable PowerPC
ARCH := $(shell test -x /usr/bin/machine -a `/usr/bin/machine` = ppc970 && echo -arch ppc64)
RUNTIME_CFLAGS += $(ARCH)
# the -D_POSIX_C_SOURCE flag prevents nonstandard functions from polluting the global name space
GEN_CFLAGS += -D_POSIX_C_SOURCE $(ARCH)
GEN_LFLAGS += $(ARCH)
endif

#
# a hacky flag necessary currently due to our use of setenv in the runtime code
#
SUPPORT_SETENV_CFLAGS = -std=gnu89

#
# Flags for turning on warnings for C++/C code
#
WARN_CXXFLAGS = -Wall -Werror -Wpointer-arith -Wwrite-strings
WARN_CFLAGS = $(WARN_CXXFLAGS) -Wmissing-declarations -Wmissing-prototypes -Wstrict-prototypes -Wnested-externs -Wdeclaration-after-statement -Wmissing-format-attribute
WARN_GEN_CFLAGS = $(WARN_CFLAGS) -Wno-unused

#
# developer settings
#
ifdef CHPL_DEVELOPER
COMP_CFLAGS += $(WARN_CXXFLAGS)
RUNTIME_CFLAGS += $(WARN_CFLAGS)
RUNTIME_GEN_CFLAGS += -Wno-unused
# GEN_CFLAGS gets warnings added via WARN_GEN_CFLAGS in comp-generated Makefile
endif
