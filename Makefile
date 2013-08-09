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

VERSION := 1.12.18

INSTALL_BIN := install --mode=755
INSTALL_DIR := install -d
INSTALL_DTA := install --mode=644

DOXYGEN     := doxygen

# Allow "make clean" (and similar non-compile targets) to work outside
# the sdk chroot without warnings about missing pkg-config files
PKG_CONFIG_NOT_REQUIRED += doc
PKG_CONFIG_NOT_REQUIRED += mostlyclean
PKG_CONFIG_NOT_REQUIRED += clean
PKG_CONFIG_NOT_REQUIRED += distclean
PKG_CONFIG_NOT_REQUIRED += tags
PKG_CONFIG_NOT_REQUIRED += fixme
PKG_CONFIG_NOT_REQUIRED += tarball
PKG_CONFIG_NOT_REQUIRED += tarball_from_git

ifneq ($(MAKECMDGOALS),)
ifeq ($(filter $(PKG_CONFIG_NOT_REQUIRED),$(MAKECMDGOALS)),$(MAKECMDGOALS))
PKG_CONFIG   := true
endif
endif
PKG_CONFIG   ?= pkg-config

# Whether to enable support for libhybris plugin
ENABLE_HYBRIS ?= y

# Whether to enable wakelock compatibility code
ENABLE_WAKELOCKS ?= y

# Whether to enable cpu scaling governor policy
ENABLE_CPU_GOVERNOR ?= y

# Whether to enable sysinfod queries
ENABLE_SYSINFOD_QUERIES ?= n

# Whether to use builtin-gconf instead of the real thing
ENABLE_BUILTIN_GCONF ?= y

# Whether to install systemd control files
ENABLE_SYSTEMD_SUPPORT ?= y

# Whether to install man pages
ENABLE_MANPAGE_INSTALL ?= y

# Whether to install restore-factory-settings and
# clear-user-data control files
ENABLE_RFS_CUD_SUPPORT ?= n

# Whether to enable backup/restore support
ENABLE_BACKUP_SUPPORT ?= n

# Whether to enable double-click == double-tap emulation
ENABLE_DOUBLETAP_EMULATION ?= y

# Install destination
DESTDIR               ?= /tmp/test-mce-install

# Standard directories
_PREFIX         ?= /usr#                         # /usr
_INCLUDEDIR     ?= $(_PREFIX)/include#           # /usr/include
_EXEC_PREFIX    ?= $(_PREFIX)#                   # /usr
_BINDIR         ?= $(_EXEC_PREFIX)/bin#          # /usr/bin
_SBINDIR        ?= $(_EXEC_PREFIX)/sbin#         # /usr/sbin
_LIBEXECDIR     ?= $(_EXEC_PREFIX)/libexec#      # /usr/libexec
_LIBDIR         ?= $(_EXEC_PREFIX)/lib#          # /usr/lib
_SYSCONFDIR     ?= /etc#                         # /etc
_DATADIR        ?= $(_PREFIX)/share#             # /usr/share
_MANDIR         ?= $(_DATADIR)/man#              # /usr/share/man
_INFODIR        ?= $(_DATADIR)/info#             # /usr/share/info
_DEFAULTDOCDIR  ?= $(_DATADIR)/doc#              # /usr/share/doc
_LOCALSTATEDIR  ?= /var#                         # /var
_UNITDIR        ?= /lib/systemd/system#

# Install directories within DESTDIR
VARDIR                := $(_LOCALSTATEDIR)/lib/mce
RUNDIR                := $(_LOCALSTATEDIR)/run/mce
CONFDIR               := $(_SYSCONFDIR)/mce
MODULEDIR             := $(_LIBDIR)/mce/modules
DBUSDIR               := $(_SYSCONFDIR)/dbus-1/system.d
LOCALEDIR             := $(_DATADIR)/locale
ifneq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
GCONFSCHEMADIR        := $(_SYSCONFDIR)/gconf/schemas
endif
BACKUPCONFDIR         := $(_DATADIR)/backup-framework/applications
HELPERSCRIPTDIR       := $(_DATADIR)/mce
DEVICECLEARSCRIPTDIR  := $(_SYSCONFDIR)/osso-cud-scripts
FACTORYRESETSCRIPTDIR := $(_SYSCONFDIR)/osso-rfs-scripts

# Source directories
DOCDIR     := doc
TOOLDIR    := tools
TESTSDIR   := tests
MODULE_DIR := modules

# Binaries to build
TARGETS += mce

# Plugins to build
MODULES += $(MODULE_DIR)/radiostates.so
MODULES += $(MODULE_DIR)/filter-brightness-als.so
MODULES += $(MODULE_DIR)/filter-brightness-simple.so
MODULES += $(MODULE_DIR)/proximity.so
MODULES += $(MODULE_DIR)/keypad.so
MODULES += $(MODULE_DIR)/inactivity.so
MODULES += $(MODULE_DIR)/camera.so
MODULES += $(MODULE_DIR)/alarm.so
MODULES += $(MODULE_DIR)/battery.so
MODULES += $(MODULE_DIR)/display.so
MODULES += $(MODULE_DIR)/led.so
MODULES += $(MODULE_DIR)/callstate.so
MODULES += $(MODULE_DIR)/audiorouting.so
MODULES += $(MODULE_DIR)/powersavemode.so
MODULES += $(MODULE_DIR)/cpu-keepalive.so

# Tools to build
TOOLS   += $(TOOLDIR)/mcetool
TOOLS   += $(TOOLDIR)/evdev_trace

# Testapps to build
TESTS   += $(TESTSDIR)/mcetorture

# MCE configuration files
CONFFILE              := 10mce.ini
RADIOSTATESCONFFILE   := 20mce-radio-states.ini
DBUSCONF              := mce.conf
ifneq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
GCONFSCHEMAS          := display.schemas energymanagement.schemas
endif

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

ifeq ($(strip $(ENABLE_CPU_GOVERNOR)),y)
CPPFLAGS += -DENABLE_CPU_GOVERNOR
endif

ifeq ($(strip $(ENABLE_SYSINFOD_QUERIES)),y)
CPPFLAGS += -DENABLE_SYSINFOD_QUERIES
endif

ifeq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
CPPFLAGS += -DENABLE_BUILTIN_GCONF
endif

ifeq ($(ENABLE_HYBRIS),y)
CPPFLAGS += -DENABLE_HYBRIS
endif

ifeq ($(ENABLE_DOUBLETAP_EMULATION),y)
CPPFLAGS += -DENABLE_DOUBLETAP_EMULATION
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

MCE_CFLAGS += -DMCE_CONF_DIR='"$(CONFDIR)"'
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
MCE_CORE += evdev.c
MCE_CORE += filewatcher.c
ifeq ($(ENABLE_HYBRIS),y)
MCE_CORE += mce-hybris.c
endif
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
ifeq ($(ENABLE_HYBRIS),y)
mce : LDLIBS += -ldl
endif
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

MODULE_CFLAGS += $(MODULE_PKG_CFLAGS)
MODULE_LDLIBS += $(MODULE_PKG_LDLIBS)

# HACK: do not link against libgconf-2
ifeq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
MODULE_LDLIBS := $(filter-out -lgconf-2, $(MODULE_LDLIBS))
endif

.PRECIOUS: %.pic.o

%.pic.o : %.c
	$(CC) -c -o $@ $< -fPIC $(CPPFLAGS) $(CFLAGS)

$(MODULE_DIR)/%.so : CFLAGS += $(MODULE_CFLAGS)
$(MODULE_DIR)/%.so : LDLIBS += $(MODULE_LDLIBS)
$(MODULE_DIR)/%.so : $(MODULE_DIR)/%.pic.o
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

$(TOOLDIR)/evdev_trace : CFLAGS += $(TOOLS_CFLAGS)
$(TOOLDIR)/evdev_trace : LDLIBS += $(TOOLS_LDLIBS)
$(TOOLDIR)/evdev_trace : $(TOOLDIR)/evdev_trace.o evdev.o

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

clean::
	$(RM) $(TARGETS) $(TOOLS) $(MODULES)

install:: build
	$(INSTALL_DIR) $(DESTDIR)$(VARDIR)
	$(INSTALL_DIR) $(DESTDIR)$(RUNDIR)

	$(INSTALL_DIR) $(DESTDIR)$(_SBINDIR)
	$(INSTALL_BIN) $(TARGETS) $(DESTDIR)$(_SBINDIR)/
	$(INSTALL_BIN) $(TOOLS)   $(DESTDIR)$(_SBINDIR)/
	$(INSTALL_BIN) $(TESTS)   $(DESTDIR)$(_SBINDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(MODULEDIR)
	$(INSTALL_BIN) $(MODULES) $(DESTDIR)$(MODULEDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(DBUSDIR)
	$(INSTALL_DTA) $(DBUSCONF) $(DESTDIR)$(DBUSDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(CONFDIR)
	$(INSTALL_DTA) inifiles/mce.ini $(DESTDIR)$(CONFDIR)/$(CONFFILE)
	$(INSTALL_DTA) inifiles/mce-radio-states.ini $(DESTDIR)$(CONFDIR)/$(RADIOSTATESCONFFILE)
	$(INSTALL_DTA) inifiles/hybris-led.ini $(DESTDIR)$(CONFDIR)/20hybris-led.ini

ifneq ($(strip $(ENABLE_BUILTIN_GCONF)),y)
	$(INSTALL_DIR) $(DESTDIR)$(GCONFSCHEMADIR)
	$(INSTALL_DTA) $(GCONFSCHEMAS) $(DESTDIR)$(GCONFSCHEMADIR)/
endif

ifeq ($(ENABLE_BACKUP_SUPPORT),y)
install:: install_backup_support
endif

install_backup_support::
	$(INSTALL_DIR) $(DESTDIR)$(BACKUPCONFDIR)
	$(INSTALL_DTA) $(BACKUPCONF) $(DESTDIR)$(BACKUPCONFDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(HELPERSCRIPTDIR)
	$(INSTALL_BIN) $(BACKUPSCRIPTS) $(DESTDIR)$(HELPERSCRIPTDIR)/

ifeq ($(ENABLE_RFS_CUD_SUPPORT),y)
install:: install_rfs_cud_support
endif

install_rfs_cud_support::
	$(INSTALL_DIR) $(DESTDIR)$(HELPERSCRIPTDIR)
	$(INSTALL_BIN) $(PRIVILEGEDDEVICECLEARSCRIPT) $(DESTDIR)$(HELPERSCRIPTDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(DEVICECLEARSCRIPTDIR)
	$(INSTALL_BIN) $(REGULARDEVICECLEARSCRIPT) $(DESTDIR)$(DEVICECLEARSCRIPTDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(FACTORYRESETSCRIPTDIR)
	$(INSTALL_BIN) $(REGULARDEVICECLEARSCRIPT) $(DESTDIR)$(FACTORYRESETSCRIPTDIR)/

ifeq ($(ENABLE_SYSTEMD_SUPPORT),y)
install:: install_systemd_support
endif

install_systemd_support::
	$(INSTALL_DIR) $(DESTDIR)$(_UNITDIR)/multi-user.target.wants/
	ln -s ../mce.service $(DESTDIR)$(_UNITDIR)/multi-user.target.wants/mce.service

	$(INSTALL_DTA) -D systemd/mce.service $(DESTDIR)$(_UNITDIR)/mce.service
	$(INSTALL_DTA) -D systemd/mce.conf    $(DESTDIR)$(_SYSCONFDIR)/tmpfiles.d/mce.conf

ifeq ($(ENABLE_MANPAGE_INSTALL),y)
install:: install_man_pages
endif

install_man_pages::
	$(INSTALL_DIR) $(DESTDIR)/$(_MANDIR)/man8
	$(INSTALL_DTA) man/mce.8        $(DESTDIR)/$(_MANDIR)/man8/mce.8
	$(INSTALL_DTA) man/mcetool.8    $(DESTDIR)/$(_MANDIR)/man8/mcetool.8
	$(INSTALL_DTA) man/mcetorture.8 $(DESTDIR)/$(_MANDIR)/man8/mcetorture.8

install_man_pages_sv::
	$(INSTALL_DIR) $(DESTDIR)/$(_MANDIR)/sv/man8
	$(INSTALL_DTA) man/mce.sv.8        $(DESTDIR)/$(_MANDIR)/sv/man8/mce.8
	$(INSTALL_DTA) man/mcetool.sv.8    $(DESTDIR)/$(_MANDIR)/sv/man8/mcetool.8
	$(INSTALL_DTA) man/mcetorture.sv.8 $(DESTDIR)/$(_MANDIR)/sv/man8/mcetorture.8

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

# ----------------------------------------------------------------------------
# LOCAL RPMBUILD (copy mce.* from OBS to rpm subdir)
# ----------------------------------------------------------------------------

# The spec file expects to find a tarball with version ...
TARBALL=mce-$(VERSION).tar
# .. that unpacks to a directory without the version.
TARWORK=mce

.PHONY: tarball
tarball:: distclean
	$(RM) -r /tmp/$(TARWORK)
	mkdir /tmp/$(TARWORK)
	tar -cf - . --exclude=.git --exclude=.gitignore --exclude=rpm |\
	tar -xf - -C /tmp/$(TARWORK)/
	tar -cjf $(TARBALL).bz2 -C /tmp $(TARWORK)/
	$(RM) -r /tmp/$(TARWORK)


.PHONY: tarball_from_git
tarball_from_git::
	git archive --prefix=mce/ -o $(TARBALL) HEAD
	bzip2 $(TARBALL)

clean::
	$(RM) $(TARBALL).bz2

.PHONY: rpmbuild
rpmbuild:: tarball
	@test -d rpm || (echo "you need rpm/ subdir for this to work" && false)
	install -m644 $(TARBALL).bz2 rpm/mce.* ~/rpmbuild/SOURCES/
	rpmbuild -ba ~/rpmbuild/SOURCES/mce.spec

