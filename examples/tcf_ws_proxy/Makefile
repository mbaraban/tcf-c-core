ROOT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
TCF_AGENT_DIR := ../../agent
include $(TCF_AGENT_DIR)/Makefile.inc

TCF_WS_PROXY_ROOT_DIR = ../tcf_ws_proxy
TCF_PROPRIETARY_ROOT_DIR = ../../wr_extensions/agent
NOPOLL_VER=0.2.7.b164
NOPOLL_DIR=$(BINDIR)/nopoll-$(NOPOLL_VER)
NOPOLL_TAR=$(ROOT_DIR)/$(TCF_WS_PROXY_ROOT_DIR)/nopoll-$(NOPOLL_VER).tar.gz
NOPOLL_LIB=$(NOPOLL_DIR)/src/.libs/libnopoll.a

INCDIRS+=$(TCF_PROPRIETARY_ROOT_DIR) $(TCF_AGENT_DIR)
override CFLAGS += $(foreach dir,$(INCDIRS),-I$(dir)) $(OPTS)

vpath %.c $(TCF_PROPRIETARY_ROOT_DIR)

HFILES := $(foreach dir,$(SRCDIRS),$(wildcard $(dir)/*.h)) $(HFILES) $(wildcard $(NOPOLL_DIR)/src/nopoll.h) $(wildcard $(TCF_PROPRIETARY_ROOT_DIR)/tcf/services/*.h) $(wildcard $(TCF_PROPRIETARY_ROOT_DIR)/tcf/framework/*.h)
CFILES := $(sort $(foreach dir,$(SRCDIRS),$(wildcard $(dir)/*.c)) $(CFILES))
CFILES := $(sort $(wildcard tcf/main/*.c) $(CFILES))
CFILES := $(sort $(foreach fnm,$(wildcard $(TCF_PROPRIETARY_ROOT_DIR)/tcf/services/*.c),$(subst ^$(TCF_PROPRIETARY_ROOT_DIR)/,,^$(fnm)))  $(CFILES))
CFILES := $(sort $(foreach fnm,$(wildcard $(TCF_PROPRIETARY_ROOT_DIR)/tcf/framework/*.c),$(subst ^$(TCF_PROPRIETARY_ROOT_DIR)/,,^$(fnm)))  $(CFILES))

EXEC = $(BINDIR)/tcf_ws_proxy$(EXTEXE)

all:    $(EXEC)

make_test:
	@echo HFILES: $(HFILES)
	@echo CFILES: $(CFILES)

$(NOPOLL_DIR)/config.h: $(NOPOLL_TAR) Makefile
	@$(RMDIR) $(NOPOLL_DIR)
	@$(MKDIR) $(BINDIR)
	cd $(BINDIR) && tar zxvf $(NOPOLL_TAR)
	cd $(NOPOLL_DIR) && ./configure $(CONFIGURE_FLAGS)

.PHONY: $(NOPOLL_LIB)

$(NOPOLL_LIB): $(NOPOLL_DIR)/config.h Makefile
ifeq ($(CONF),Debug)
	cd $(NOPOLL_DIR) && make CFLAGS="-g"
else
	cd $(NOPOLL_DIR) && make
endif

$(NOPOLL_DIR)/src/nopoll.h: $(NOPOLL_LIB)

$(BINDIR)/libtcf$(EXTLIB) : $(OFILES)
	$(AR) $(AR_FLAGS) $@ $^

$(EXEC): $(NOPOLL_LIB) $(BINDIR)/tcf/main/main$(EXTOBJ) $(BINDIR)/libtcf$(EXTLIB)
	$(CC) $(CFLAGS) -o $@ $(BINDIR)/tcf/main/main$(EXTOBJ) $(BINDIR)/libtcf$(EXTLIB) $(LIBS) $(NOPOLL_LIB)

$(BINDIR)/%$(EXTOBJ): %.c $(HFILES) Makefile $(NOPOLL_DIR)/src/nopoll.h
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -I$(NOPOLL_DIR)/src -c -o $@ $<

$(BINDIR)/%$(EXTOBJ): $(TCF_AGENT_DIR)/%.c $(HFILES) Makefile
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(EXEC)
	cp $(EXEC) $(INSTALLDIR)/

clean:
	$(call RMDIR,$(BINDIR))
