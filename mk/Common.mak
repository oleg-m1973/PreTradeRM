SUB_DIR := .

SOLUTION_DIR := $(SUB_DIR)
BUILD_MAK = $(SOLUTION_DIR)/mk/Build.mak

SOURCE_DIR = $(SOLUTION_DIR)/Source/

ifeq ($(CONFIG), )
	CONFIG = Release
endif

OUTDIR := $(SOLUTION_DIR)/build_bin/$(CONFIG)/
OBJDIR := $(SOLUTION_DIR)/build_obj/$(CONFIG)/
#LIBDIR := $(OBJDIR)_lib_/

EXTLIBDIR := \
	-L/usr/local/lib/ \

export CONFIG

#TARGET = executable
#TARGET = static-lib
#TARGET = shared-lib
