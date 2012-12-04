# Makefile for MCE
# Copyright © 2004-2011 Nokia Corporation.
# Written by David Weinehall
# Modified by Tuomo Tanskanen
# Modified by Simo Piiroinen
#
# mce is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License
# version 2.1 as published by the Free Software Foundation.
#
# mce is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with mce.  If not, see <http://www.gnu.org/licenses/>.

# ----------------------------------------------------------------------------
# TOP LEVEL TARGETS
# ----------------------------------------------------------------------------

.PHONY: build modules tools doc install clean distclean mostlyclean

build::

modules::

tools::

doc::

install::

mostlyclean::
	$(RM) *.o *.bak *~ */*.o */*.bak */*~

clean:: mostlyclean

distclean:: clean

# ----------------------------------------------------------------------------
# CONFIGURATION
# ----------------------------------------------------------------------------

VERSION := 1.12.5

INSTALL_BIN := install --mode=755
INSTALL_DIR := install -d
INSTALL_DTA := install --mode=644

DOXYGEN     := doxygen

# allow "make clean" outside sdk chroot to work without warnings
# about missing pkg-config files
ifeq ($(MAKECMDGOALS),clean)
PKG_CONFIG   := true
endif
PKG_CONFIG   ?= pkg-config

# Whether to enable wakelock compatibility code
ENABLE_WAKELOCKS ?= n

# Whether to enable sysinfod queries
ENABLE_SYSINFOD_QUERIES ?= n

# Whether to use builtin-gconf instead of the real thing
ENABLE_BUILTIN_GCONF ?= y

# Install directories
VARDIR                := $(DESTDIR)/var/lib/mce
RUNDIR                := $(DESTDIR)/var/run/mce
CONFDIR               := /etc/mce
CONFINSTDIR           := $(DESTDIR)$(CONFDIR)
SBINDIR               := $(DESTDIR)/sbin
MODULEDIR             := $(DESTDIR)/usr/lib/mce/modules
DBUSDIR               := $(DESTDIR)/etc/dbus-1/system.d
LOCALEDIR             := $(DESTDIR)/usr/share/locale
GCONFSCHEMADIR        := $(DESTDIR)/etc/gconf/schemas
BACKUPCONFDIR         := $(DESTDIR)/usr/share/backup-framework/applications
HELPERSCRIPTDIR       := $(DESTDIR)/usr/share/mce
DEVICECLEARSCRIPTDIR  := $(DESTDIR)/etc/osso-cud-scripts
FACTORYRESETSCRIPTDIR := $(DESTDIR)/etc/osso-rfs-scripts

# Source directories
TOPDIR     := .
DOCDIR     := $(TOPDIR)/doc
TOOLDIR    := $(TOPDIR)/tools
TESTSDIR   := $(TOPDIR)/tests
MODULE_DIR := $(TOPDIR)/modules

# Binaries to build
TARGETS += mce

# Plugins to build
MODULES += $(MODULE_DIR)/libradiostates.so
MODULES += $(MODULE_DIR)/libfilter-brightness-als.so
MODULES += $(MODULE_DIR)/libfilter-brightness-simple.so
MODULES += $(MODULE_DIR)/libproximity.so
MODULES += $(MODULE_DIR)/libkeypad.so
MODULES += $(MODULE_DIR)/libinactivity.so
MODULES += $(MODULE_DIR)/libcamera.so
MODULES += $(MODULE_DIR)/libalarm.so
MODULES += $(MODULE_DIR)/libbattery.so
MODULES += $(MODULE_DIR)/libdisplay.so
MODULES += $(MODULE_DIR)/libled.so
MODULES += $(MODULE_DIR)/libcallstate.so
MODULES += $(MODULE_DIR)/libaudiorouting.so
MODULES += $(MODULE_DIR)/libpowersavemode.so

# Tools to build
TOOLS   += $(TOOLDIR)/mcetool

# Testapps to build
TESTS   += $(TESTSDIR)/mcetorture

# MCE configuration files
CONFFILE              := mce.ini
RADIOSTATESCONFFILE   := mce-radio-states.ini
COLORPROFILESCONFFILE := mce-color-profiles.ini
DBUSCONF              := mce.conf
GCONFSCHEMAS          := display.schemas energymanagement.schemas

# Backup / Restore
BACKUPCONF            := mcebackup.conf
BACKUPSCRIPTS         := mce-backup mce-restore

# Restore factory settings / clear user data
PRIVILEGEDDEVICECLEARSCRIPT := mce-device-clear
REGULARDEVICECLEARSCRIPT    := mce-device-clear.sh

# ----------------------------------------------------------------------------
# DEFAULT FLAGS
# ----------------------------------------------------------------------------

# C Preprocessor
CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -I.
CPPFLAGS += -DG_DISABLE_DEPRECATED
CPPFLAGS += -DOSSOLOG_COMPILE
CPPFLAGS += -DMCE_VAR_DIR=$(VARDIR)
CPPFLAGS += -DMCE_RUN_DIR=$(RUNDIR)
CPPFLAGS += -DPRG_VERSION=$(VERSION)

ifeq ($(strip $(ENABLE_WAKELOCKS)),y)
CPPFLAGS += -DENABLE_WAKELOCKS
endif

ifeq ($(strip $(ENABLE_SYSINFOD_QUERIES)),y)
CPPFLAGS += -DENABLE_SYSINFOD_QUERIES
endif

ifeq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
CPPFLAGS += -DENABLE_BUILTIN_GCONF
endif

# C Compiler
CFLAGS += -std=c99

# Do we really need all of these?
CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Wpointer-arith
CFLAGS += -Wundef
CFLAGS += -Wcast-align
CFLAGS += -Wshadow
CFLAGS += -Wbad-function-cast
CFLAGS += -Wwrite-strings
CFLAGS += -Wsign-compare
CFLAGS += -Waggregate-return
CFLAGS += -Wmissing-noreturn
CFLAGS += -Wnested-externs
#CFLAGS += -Wchar-subscripts (-Wall does this)
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wformat-security
CFLAGS += -Wformat=2
CFLAGS += -Wformat-nonliteral
CFLAGS += -Winit-self
CFLAGS += -Wswitch-default
CFLAGS += -Wstrict-prototypes
CFLAGS += -Wdeclaration-after-statement
CFLAGS += -Wold-style-definition
CFLAGS += -Wmissing-declarations
CFLAGS += -Wmissing-include-dirs
CFLAGS += -Wstrict-aliasing=2
CFLAGS += -Wunsafe-loop-optimizations
CFLAGS += -Winvalid-pch
#CFLAGS += -Waddress  (-Wall does this)
CFLAGS += -Wvolatile-register-var
CFLAGS += -Wmissing-format-attribute
CFLAGS += -Wstack-protector
#CFLAGS += -Werror (OBS build might have different compiler)
CFLAGS += -Wno-declaration-after-statement

# Linker
LDLIBS   += -Wl,--as-needed

# ----------------------------------------------------------------------------
# MCE
# ----------------------------------------------------------------------------

MCE_PKG_NAMES += gobject-2.0
MCE_PKG_NAMES += glib-2.0
MCE_PKG_NAMES += gio-2.0
MCE_PKG_NAMES += gmodule-2.0
MCE_PKG_NAMES += dbus-1
MCE_PKG_NAMES += dbus-glib-1
MCE_PKG_NAMES += gconf-2.0
MCE_PKG_NAMES += dsme

MCE_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(MCE_PKG_NAMES))
MCE_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(MCE_PKG_NAMES))

MCE_CFLAGS += -DMCE_CONF_FILE=$(CONFDIR)/$(CONFFILE)
MCE_CFLAGS += $(MCE_PKG_CFLAGS)

MCE_LDLIBS += $(MCE_PKG_LDLIBS)

# These must be made callable from the plugins
MCE_CORE += tklock.c
MCE_CORE += modetransition.c
MCE_CORE += powerkey.c
MCE_CORE += mce-dbus.c
MCE_CORE += mce-dsme.c
MCE_CORE += mce-gconf.c
MCE_CORE += event-input.c
MCE_CORE += event-switches.c
MCE_CORE += mce-hal.c
MCE_CORE += mce-log.c
MCE_CORE += mce-conf.c
MCE_CORE += datapipe.c
MCE_CORE += mce-modules.c
MCE_CORE += mce-io.c
MCE_CORE += mce-lib.c
MCE_CORE += median_filter.c

# HACK: do not link against libgconf-2
ifeq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
MCE_CORE   += builtin-gconf.c
MCE_LDLIBS := $(filter-out -lgconf-2, $(MCE_LDLIBS))
endif

ifeq ($(strip $(ENABLE_WAKELOCKS)),y)
MCE_CORE   += libwakelock.c
endif

mce : CFLAGS += $(MCE_CFLAGS)
mce : LDLIBS += $(MCE_LDLIBS)
mce : mce.o $(patsubst %.c,%.o,$(MCE_CORE))

# ----------------------------------------------------------------------------
# MODULES
# ----------------------------------------------------------------------------

MODULE_PKG_NAMES += gobject-2.0
MODULE_PKG_NAMES += glib-2.0
MODULE_PKG_NAMES += gmodule-2.0
MODULE_PKG_NAMES += dbus-1
MODULE_PKG_NAMES += dbus-glib-1
MODULE_PKG_NAMES += gconf-2.0

MODULE_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(MODULE_PKG_NAMES))
MODULE_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(MODULE_PKG_NAMES))

MODULE_CFLAGS += -DMCE_RADIO_STATES_CONF_FILE=$(CONFDIR)/$(RADIOSTATESCONFFILE)
MODULE_CFLAGS += -DMCE_COLOR_PROFILES_CONF_FILE=$(CONFDIR)/$(COLORPROFILESCONFFILE)

MODULE_CFLAGS += $(MODULE_PKG_CFLAGS)
MODULE_LDLIBS += $(MODULE_PKG_LDLIBS)

# HACK: do not link against libgconf-2
ifeq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
MODULE_LDLIBS := $(filter-out -lgconf-2, $(MODULE_LDLIBS))
endif

.PRECIOUS: %.pic.o

%.pic.o : %.c
	$(CC) -c -o $@ $< -fPIC $(CPPFLAGS) $(CFLAGS)

$(MODULE_DIR)/lib%.so : CFLAGS += $(MODULE_CFLAGS)
$(MODULE_DIR)/lib%.so : LDLIBS += $(MODULE_LDLIBS)
$(MODULE_DIR)/lib%.so : $(MODULE_DIR)/%.pic.o
	$(CC) -shared -o $@ $^ $(LDFLAGS) $(LDLIBS)

# ----------------------------------------------------------------------------
# TOOLS
# ----------------------------------------------------------------------------

TOOLS_PKG_NAMES += gobject-2.0
TOOLS_PKG_NAMES += glib-2.0
TOOLS_PKG_NAMES += dbus-1
TOOLS_PKG_NAMES += gconf-2.0

TOOLS_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(TOOLS_PKG_NAMES))
TOOLS_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(TOOLS_PKG_NAMES))

TOOLS_CFLAGS += $(TOOLS_PKG_CFLAGS)
TOOLS_LDLIBS += $(TOOLS_PKG_LDLIBS)

# HACK: do not link against libgconf-2
ifeq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
TOOLS_LDLIBS := $(filter-out -lgconf-2, $(TOOLS_LDLIBS))
endif

$(TOOLDIR)/mcetool : CFLAGS += $(TOOLS_CFLAGS)
$(TOOLDIR)/mcetool : LDLIBS += $(TOOLS_LDLIBS)
$(TOOLDIR)/mcetool : $(TOOLDIR)/mcetool.o

# ----------------------------------------------------------------------------
# TESTS
# ----------------------------------------------------------------------------

$(TESTSDIR)/mcetorture : $(TESTSDIR)/mcetorture.o

# ----------------------------------------------------------------------------
# ACTIONS FOR TOP LEVEL TARGETS
# ----------------------------------------------------------------------------

build:: $(TARGETS) $(MODULES) $(TOOLS)

modules:: $(MODULES)

tools:: $(TOOLS)

install:: build
	$(INSTALL_DIR) $(SBINDIR) $(DBUSDIR) $(VARDIR) $(MODULEDIR)
	$(INSTALL_DIR) $(RUNDIR) $(CONFINSTDIR) $(GCONFSCHEMADIR)
	$(INSTALL_DIR) $(BACKUPCONFDIR) $(HELPERSCRIPTDIR)
	$(INSTALL_DIR) $(DEVICECLEARSCRIPTDIR) $(FACTORYRESETSCRIPTDIR)
	$(INSTALL_BIN) $(TARGETS) $(SBINDIR)
	$(INSTALL_BIN) $(TOOLS) $(TESTS) $(SBINDIR)
	$(INSTALL_BIN) $(MODULES) $(MODULEDIR)
	$(INSTALL_BIN) $(BACKUPSCRIPTS) $(HELPERSCRIPTDIR)
	$(INSTALL_BIN) $(PRIVILEGEDDEVICECLEARSCRIPT) $(HELPERSCRIPTDIR)
	$(INSTALL_BIN) $(REGULARDEVICECLEARSCRIPT) $(DEVICECLEARSCRIPTDIR)
	$(INSTALL_BIN) $(REGULARDEVICECLEARSCRIPT) $(FACTORYRESETSCRIPTDIR)
	$(INSTALL_DTA) $(CONFFILE) $(CONFINSTDIR)
	$(INSTALL_DTA) $(RADIOSTATESCONFFILE) $(CONFINSTDIR)
	$(INSTALL_DTA) $(GCONFSCHEMAS) $(GCONFSCHEMADIR)
	$(INSTALL_DTA) $(DBUSCONF) $(DBUSDIR)
	$(INSTALL_DTA) $(BACKUPCONF) $(BACKUPCONFDIR)

clean::
	$(RM) $(TARGETS) $(TOOLS) $(MODULES)

# ----------------------------------------------------------------------------
# DOCUMENTATION
# ----------------------------------------------------------------------------

doc::
	mkdir -p doc
	$(DOXYGEN) > /dev/null # 2> $(DOCDIR)/warnings

clean::
	$(RM) -r doc # in case DOCDIR points somewhere funny ...

# ----------------------------------------------------------------------------
# CTAGS
# ----------------------------------------------------------------------------

.PHONY: tags
tags::
	find . $(MODULE_DIR) -maxdepth 1 -type f -name '*.[ch]' | xargs ctags -a --extra=+f

distclean::
	$(RM) tags

# ----------------------------------------------------------------------------
# FIXME
# ----------------------------------------------------------------------------

.PHONY: fixme
fixme::
	find . -type f -name "*.[ch]" | xargs grep -E "(FIXME|XXX|TODO)"

# ----------------------------------------------------------------------------
# AUTOMATIC HEADER DEPENDENCIES
# ----------------------------------------------------------------------------

.PHONY: depend
depend::
	@echo "Updating .depend"
	$(CC) -MM $(CPPFLAGS) $(MCE_CFLAGS) *.c */*.c |\
	./depend_filter.py > .depend

ifneq ($(MAKECMDGOALS),depend) # not while: make depend
ifneq (,$(wildcard .depend))   # not if .depend does not exist
include .depend
endif
endif
