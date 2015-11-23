# Makefile - Top level GNU Makefile for VxWorks7 stop mode agent
#
# Copyright (c) 2012-2014 Wind River Systems, Inc.
#
# The right to copy, distribute, modify or otherwise make use
# of this software may be licensed only pursuant to the terms
# of an applicable Wind River license agreement.
#

# To build the VxWorks7 stop mode agent:
# 	make DEBUG=1|0
# Build objects will be stored under obj directory
#

EXEC_NAME               = appvirtgw

.PHONY:	all $(EXEC_NAME) install rclean clean

# By default, build agent


default: $(EXEC_NAME)

ifndef	OPSYS
OPSYS			:= $(shell uname -o 2>/dev/null || uname -s)
endif
ALLOW_COMPILE_WARNINGS  ?= 0
BUILD_MAKEFLAGS         := --no-print-directory
OBJDIR			= obj
OUTDIR			= .
BUILD_MAKE		= $(MAKE)

ifeq ($(OPSYS),Msys)
CMAKE_GENERATOR		:= "MSYS Makefiles"
else
ifeq ($(OPSYS),MinGW)
CMAKE_GENERATOR		:= "MinGW Makefiles"
else
ifeq ($(OPSYS),Windows-MSVC)
CMAKE_GENERATOR		:= "NMake Makefiles"
BUILD_MAKE		:= nmake
BUILD_MAKEFLAGS		=
override MAKEFLAGS	=
else
CMAKE_GENERATOR		:= "Unix Makefiles"
endif
endif
endif

ifeq ("$(CONF)","Release")
CMAKE_BUILD_TYPE	= Release
else
ifeq ("$(CONF)","Debug")
CMAKE_BUILD_TYPE	= Debug
else
CMAKE_BUILD_TYPE	= Release
endif
endif
BUILD_TYPE              := $(shell echo $(CMAKE_BUILD_TYPE) | tr '[A-Z]' '[a-z]')
CMAKE_ARGS		=  -DALLOW_COMPILE_WARNINGS=$(ALLOW_COMPILE_WARNINGS) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)

OBJDIR			= obj

ifneq ($(C_COMPILER),)
CMAKE_ARGS              += -DCMAKE_C_COMPILER=$(C_COMPILER)
endif
ifneq ($(CXX_COMPILER),)
CMAKE_ARGS              += -DCMAKE_CXX_COMPILER=$(CXX_COMPILER)
endif
ifneq ($(DEVICE_SERVER_NAME),)
CMAKE_ARGS              += -DDEVICE_SERVER_NAME="$(DEVICE_SERVER_NAME)"
endif

ifeq ($(TRACE),1)
CMAKE_ARGS += --trace
endif
CMAKE_ARGS += -DEXEC_NAME=$(EXEC_NAME)

BUILD_DIR		= $(OUTDIR)/$(OBJDIR)/$(OPSYS)/$(BUILD_TYPE)

ifdef CMAKE_GENERATOR_VS
BUILD_DIR_VS		= $(OUTDIR)/$(OBJDIR)/$(OPSYS)/$(BUILD_TYPE)_VS
endif

$(BUILD_DIR)/Makefile: Makefile
	@mkdir -p $(BUILD_DIR)
	(cd $(BUILD_DIR) && cmake -G $(CMAKE_GENERATOR) $(CMAKE_ARGS) $(CURDIR))
ifdef CMAKE_GENERATOR_VS	
	@mkdir -p $(BUILD_DIR_VS)
	-(cd $(BUILD_DIR_VS) && cmake -G $(CMAKE_GENERATOR_VS) $(CMAKE_ARGS) $(CURDIR))
endif

$(EXEC_NAME): $(BUILD_DIR)/Makefile
	echo building $(EXEC_NAME)
	(cd $(BUILD_DIR) && $(BUILD_MAKE) $(BUILD_MAKEFLAGS) rebuild_cache all)

rclean clean: 
	rm -rf $(BUILD_DIR)
ifdef BUILD_DIR_VS
	rm -rf $(BUILD_DIR_VS)
endif	
install: $(EXEC_NAME)
	cp $(BUILD_DIR)/$(EXEC_NAME) $(INSTALLDIR)/
