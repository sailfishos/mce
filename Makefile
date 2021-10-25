# Makefile for MCE
# Copyright © 2004-2011 Nokia Corporation.
# Copyright (C) 2012-2019 Jolla Ltd.
#
# @author David Weinehall <david.weinehall@nokia.com>
# @author Tuomo Tanskanen
# @author Irina Bezruk <ext-irina.bezruk@nokia.com>
# @author Victor Portnov <ext-victor.portnov@nokia.com>
# @author Jukka Turunen <ext-jukka.t.turunen@nokia.com>
# @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
# @author Tapio Rantala <ext-tapio.rantala@nokia.com>
# @author Dmitry Rozhkov <dmitry.rozhkov@jollamobile.com>
# @author Kalle Jokiniemi <kalle.jokiniemi@jolla.com>
# @author Philippe De Swert <philippe.deswert@jollamobile.com>
# @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
# @author Martin Kampas <martin.kampas@tieto.com>
# @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

.PHONY: build modules tools check doc install clean distclean mostlyclean

build::

modules::

tools::

check::

doc::

install::

mostlyclean::
	$(RM) *.o *.bak *~ */*.o */*.bak */*~ */*/*.o */*/*.bak */*/*~

clean:: mostlyclean

distclean:: clean

# ----------------------------------------------------------------------------
# CONFIGURATION
# ----------------------------------------------------------------------------

VERSION := 1.109.0

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
PKG_CONFIG_NOT_REQUIRED += normalize
PKG_CONFIG_NOT_REQUIRED += tarball
PKG_CONFIG_NOT_REQUIRED += tarball_from_git

ifneq ($(MAKECMDGOALS),)
ifeq ($(filter $(PKG_CONFIG_NOT_REQUIRED),$(MAKECMDGOALS)),$(MAKECMDGOALS))
PKG_CONFIG   := true
endif
endif
PKG_CONFIG   ?= pkg-config

# Whether to enable DEVEL release logging
ENABLE_DEVEL_LOGGING ?= y

# Whether to enable support for libhybris plugin
ENABLE_HYBRIS ?= y

# Whether to enable wakelock compatibility code
ENABLE_WAKELOCKS ?= y

# Whether to enable cpu scaling governor policy
ENABLE_CPU_GOVERNOR ?= y

# Whether to install systemd control files
ENABLE_SYSTEMD_SUPPORT ?= y

# Whether to install man pages
ENABLE_MANPAGE_INSTALL ?= y

# Whether to enable double-click == double-tap emulation
ENABLE_DOUBLETAP_EMULATION ?= y

# Whether to install unit tests
ENABLE_UNITTESTS_INSTALL ?= n

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
_TESTSDIR       ?= /opt/tests#                   # /opt/tests

# Install directories within DESTDIR
VARDIR                := $(_LOCALSTATEDIR)/lib/mce
RUNDIR                := $(_LOCALSTATEDIR)/run/mce
CONFDIR               := $(_SYSCONFDIR)/mce
MODULEDIR             := $(_LIBDIR)/mce/modules
DBUSDIR               := $(_SYSCONFDIR)/dbus-1/system.d
HELPERSCRIPTDIR       := $(_DATADIR)/mce
TESTSDESTDIR          := $(_TESTSDIR)/mce

# Source directories
DOCDIR          := doc
TOOLDIR         := tools
TESTSDIR        := tests
UTESTDIR        := tests/ut
MODULE_DIR      := modules
DBUS_GMAIN_DIR  := dbus-gmain

# Binaries to build
TARGETS += mce

# Plugins to build
MODULES += $(MODULE_DIR)/radiostates.so
MODULES += $(MODULE_DIR)/filter-brightness-als.so
MODULES += $(MODULE_DIR)/fingerprint.so
MODULES += $(MODULE_DIR)/proximity.so
MODULES += $(MODULE_DIR)/keypad.so
MODULES += $(MODULE_DIR)/inactivity.so
MODULES += $(MODULE_DIR)/camera.so
MODULES += $(MODULE_DIR)/alarm.so
MODULES += $(MODULE_DIR)/memnotify.so
MODULES += $(MODULE_DIR)/mempressure.so
MODULES += $(MODULE_DIR)/battery-bme.so
MODULES += $(MODULE_DIR)/battery-upower.so
MODULES += $(MODULE_DIR)/bluetooth.so
MODULES += $(MODULE_DIR)/battery-statefs.so
MODULES += $(MODULE_DIR)/battery-udev.so
MODULES += $(MODULE_DIR)/buttonbacklight.so
MODULES += $(MODULE_DIR)/display.so
MODULES += $(MODULE_DIR)/usbmode.so
MODULES += $(MODULE_DIR)/doubletap.so
MODULES += $(MODULE_DIR)/sensor-gestures.so
MODULES += $(MODULE_DIR)/led.so
MODULES += $(MODULE_DIR)/callstate.so
MODULES += $(MODULE_DIR)/audiorouting.so
MODULES += $(MODULE_DIR)/powersavemode.so
MODULES += $(MODULE_DIR)/cpu-keepalive.so
MODULES += $(MODULE_DIR)/packagekit.so

# Tools to build
TOOLS   += $(TOOLDIR)/mcetool
TOOLS   += $(TOOLDIR)/evdev_trace
TOOLS   += $(TOOLDIR)/dummy_compositor

# Unit tests to build
UTESTS  += $(UTESTDIR)/ut_display_conf
UTESTS  += $(UTESTDIR)/ut_display_stm
UTESTS  += $(UTESTDIR)/ut_display_filter
UTESTS  += $(UTESTDIR)/ut_display_blanking_inhibit
UTESTS  += $(UTESTDIR)/ut_display

# MCE configuration files
CONFFILE              := 10mce.ini
RADIOSTATESCONFFILE   := 20mce-radio-states.ini
DBUSCONF              := mce.conf

# ----------------------------------------------------------------------------
# DEFAULT FLAGS
# ----------------------------------------------------------------------------

# C Preprocessor
CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -DG_DISABLE_DEPRECATED
CPPFLAGS += -DOSSOLOG_COMPILE
CPPFLAGS += -DMCE_VAR_DIR=$(VARDIR)
CPPFLAGS += -DMCE_RUN_DIR=$(RUNDIR)
CPPFLAGS += -DPRG_VERSION=$(VERSION)
# Default module path
CPPFLAGS += -DMCE_DEFAULT_MCE_MODULE_PATH=$(MODULEDIR)

ifeq ($(strip $(ENABLE_WAKELOCKS)),y)
CPPFLAGS += -DENABLE_WAKELOCKS
endif

ifeq ($(strip $(ENABLE_CPU_GOVERNOR)),y)
CPPFLAGS += -DENABLE_CPU_GOVERNOR
endif

ifeq ($(ENABLE_HYBRIS),y)
CPPFLAGS += -DENABLE_HYBRIS
endif

ifeq ($(ENABLE_DOUBLETAP_EMULATION),y)
CPPFLAGS += -DENABLE_DOUBLETAP_EMULATION
endif

ifeq ($(ENABLE_DEVEL_LOGGING),y)
CPPFLAGS += -DENABLE_DEVEL_LOGGING
CPPFLAGS += -DENABLE_BATTERY_SIMULATION
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
CFLAGS += -Wstrict-aliasing=3
CFLAGS += -Wunsafe-loop-optimizations
CFLAGS += -Winvalid-pch
#CFLAGS += -Waddress  (-Wall does this)
CFLAGS += -Wvolatile-register-var
CFLAGS += -Wmissing-format-attribute
CFLAGS += -Wstack-protector
#CFLAGS += -Werror (OBS build might have different compiler)
CFLAGS += -Wno-declaration-after-statement
CFLAGS += -Wno-missing-field-initializers
CFLAGS += -Wimplicit-fallthrough=2
CFLAGS += -Wno-format-truncation

# Linker
LDLIBS   += -Wl,--as-needed

# ----------------------------------------------------------------------------
# MCE
# ----------------------------------------------------------------------------

%.o : %.c
	$(CC) -c -o $@ $< $(CPPFLAGS) $(CFLAGS)

MCE_PKG_NAMES += gobject-2.0
MCE_PKG_NAMES += glib-2.0
MCE_PKG_NAMES += gio-2.0
MCE_PKG_NAMES += gmodule-2.0
MCE_PKG_NAMES += dbus-1
MCE_PKG_NAMES += dsme
MCE_PKG_NAMES += libiphb
MCE_PKG_NAMES += libsystemd
MCE_PKG_NAMES += libngf0
MCE_PKG_NAMES += thermalmanager_dbus_if

MCE_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(MCE_PKG_NAMES))
MCE_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(MCE_PKG_NAMES))

MCE_CFLAGS += -DMCE_CONF_DIR='"$(CONFDIR)"'
MCE_CFLAGS += $(MCE_PKG_CFLAGS)

MCE_LDLIBS += $(MCE_PKG_LDLIBS)

# These must be made callable from the plugins
MCE_CORE += datapipe.c
MCE_CORE += tklock.c
MCE_CORE += modetransition.c
MCE_CORE += powerkey.c
MCE_CORE += mce-common.c
MCE_CORE += mce-fbdev.c
MCE_CORE += mce-dbus.c
MCE_CORE += mce-dsme.c
MCE_CORE += mce-setting.c
MCE_CORE += mce-hbtimer.c
MCE_CORE += mce-wltimer.c
MCE_CORE += mce-wakelock.c
MCE_CORE += mce-worker.c
MCE_CORE += event-input.c
MCE_CORE += event-switches.c
MCE_CORE += mce-hal.c
MCE_CORE += mce-log.c
MCE_CORE += mce-command-line.c
MCE_CORE += mce-conf.c
MCE_CORE += mce-modules.c
MCE_CORE += multitouch.c
MCE_CORE += mce-io.c
MCE_CORE += mce-lib.c
MCE_CORE += evdev.c
MCE_CORE += filewatcher.c
ifeq ($(ENABLE_HYBRIS),y)
MCE_CORE += mce-hybris.c
endif
MCE_CORE += mce-sensorfw.c
MCE_CORE += builtin-gconf.c

ifeq ($(strip $(ENABLE_WAKELOCKS)),y)
MCE_CORE   += libwakelock.c
endif

MCE_CORE += $(DBUS_GMAIN_DIR)/dbus-gmain.c

mce : CFLAGS += $(MCE_CFLAGS)
mce : LDLIBS += $(MCE_LDLIBS)
ifeq ($(ENABLE_HYBRIS),y)
mce : LDLIBS += -ldl
endif
mce : mce.o $(patsubst %.c,%.o,$(MCE_CORE))

CFLAGS  += -g
LDFLAGS += -g

# ----------------------------------------------------------------------------
# DBUS_GMAIN
# ----------------------------------------------------------------------------

# The dbus-gmain submodule contains sources that have
# issues and do not compile cleanly. As the content is
# what it is, silence warnings etc when compiling source
# files from there...

DBUS_GMAIN_CPPFLAGS += -I.
DBUS_GMAIN_CFLAGS   += -Wno-unused-parameter
DBUS_GMAIN_CFLAGS   += -Wno-cast-function-type

$(DBUS_GMAIN_DIR)/%.o : CPPFLAGS += $(DBUS_GMAIN_CPPFLAGS)
$(DBUS_GMAIN_DIR)/%.o : CFLAGS   += $(DBUS_GMAIN_CFLAGS)

# ----------------------------------------------------------------------------
# MODULES
# ----------------------------------------------------------------------------

MODULE_PKG_NAMES += gobject-2.0
MODULE_PKG_NAMES += glib-2.0
MODULE_PKG_NAMES += gmodule-2.0
MODULE_PKG_NAMES += dbus-1
MODULE_PKG_NAMES += usb_moded
MODULE_PKG_NAMES += libudev

MODULE_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(MODULE_PKG_NAMES))
MODULE_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(MODULE_PKG_NAMES))

MODULE_CFLAGS += $(MODULE_PKG_CFLAGS)
MODULE_LDLIBS += $(MODULE_PKG_LDLIBS)

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

TOOLS_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(TOOLS_PKG_NAMES))
TOOLS_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(TOOLS_PKG_NAMES))

TOOLS_CFLAGS += $(TOOLS_PKG_CFLAGS)
TOOLS_LDLIBS += $(TOOLS_PKG_LDLIBS)

$(TOOLDIR)/mcetool : CFLAGS += $(TOOLS_CFLAGS)
$(TOOLDIR)/mcetool : LDLIBS += $(TOOLS_LDLIBS)
$(TOOLDIR)/mcetool : $(TOOLDIR)/mcetool.o mce-command-line.o

$(TOOLDIR)/evdev_trace : CFLAGS += $(TOOLS_CFLAGS)
$(TOOLDIR)/evdev_trace : LDLIBS += $(TOOLS_LDLIBS)
$(TOOLDIR)/evdev_trace : $(TOOLDIR)/evdev_trace.o evdev.o $(TOOLDIR)/fileusers.o

$(TOOLDIR)/dummy_compositor : CFLAGS += $(TOOLS_CFLAGS)
$(TOOLDIR)/dummy_compositor : LDLIBS += $(TOOLS_LDLIBS)
$(TOOLDIR)/dummy_compositor : $(TOOLDIR)/dummy_compositor.o $(DBUS_GMAIN_DIR)/dbus-gmain.o

# ----------------------------------------------------------------------------
# UNIT TESTS
# ----------------------------------------------------------------------------

UTESTS_PKG_NAMES += check
UTESTS_PKG_NAMES += dbus-1
UTESTS_PKG_NAMES += glib-2.0
UTESTS_PKG_NAMES += gthread-2.0

UTESTS_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(UTESTS_PKG_NAMES))
UTESTS_PKG_LDLIBS := $(shell $(PKG_CONFIG) --libs   $(UTESTS_PKG_NAMES))

UTESTS_CFLAGS += $(UTESTS_PKG_CFLAGS)
UTESTS_LDLIBS += $(UTESTS_PKG_LDLIBS)

UTESTS_CFLAGS += -fdata-sections -ffunction-sections
UTESTS_LDLIBS += -Wl,--gc-sections

$(UTESTDIR)/% : CFLAGS += $(UTESTS_CFLAGS)
$(UTESTDIR)/% : LDLIBS += $(UTESTS_LDLIBS)
$(UTESTDIR)/% : LDLIBS += $(foreach fn_sym,$(LINK_STUBS),\
				    -Wl,--defsym=$(fn_sym)=stub__$(fn_sym))
$(UTESTDIR)/% : $(UTESTDIR)/%.o

$(UTESTDIR)/ut_display : LINK_STUBS += mce_log_file
$(UTESTDIR)/ut_display : LINK_STUBS += mce_write_string_to_file
$(UTESTDIR)/ut_display : datapipe.o
$(UTESTDIR)/ut_display : mce-lib.o
$(UTESTDIR)/ut_display : modetransition.o
$(UTESTDIR)/ut_display : $(DBUS_GMAIN_DIR)/dbus-gmain.o

# ----------------------------------------------------------------------------
# ACTIONS FOR TOP LEVEL TARGETS
# ----------------------------------------------------------------------------

build:: $(TARGETS) $(MODULES) $(TOOLS)

ifeq ($(ENABLE_UNITTESTS_INSTALL),y)
build:: $(UTESTS)
endif

modules:: $(MODULES)

tools:: $(TOOLS)

check:: $(UTESTS)
	for utest in $^; do ./$${utest} || exit; done

clean::
	$(RM) $(TARGETS) $(TOOLS) $(MODULES)

ifeq ($(ENABLE_UNITTESTS_INSTALL),y)
	$(RM) $(UTESTS)
endif

install:: build
	$(INSTALL_DIR) $(DESTDIR)$(VARDIR)
	$(INSTALL_DIR) $(DESTDIR)$(RUNDIR)

	$(INSTALL_DIR) $(DESTDIR)$(_SBINDIR)
	$(INSTALL_BIN) $(TARGETS) $(DESTDIR)$(_SBINDIR)/
	$(INSTALL_BIN) $(TOOLS)   $(DESTDIR)$(_SBINDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(MODULEDIR)
	$(INSTALL_BIN) $(MODULES) $(DESTDIR)$(MODULEDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(DBUSDIR)
	$(INSTALL_DTA) $(DBUSCONF) $(DESTDIR)$(DBUSDIR)/

	$(INSTALL_DIR) $(DESTDIR)$(CONFDIR)
	$(INSTALL_DTA) inifiles/mce.ini $(DESTDIR)$(CONFDIR)/$(CONFFILE)
	$(INSTALL_DTA) inifiles/mce-radio-states.ini $(DESTDIR)$(CONFDIR)/$(RADIOSTATESCONFFILE)
	$(INSTALL_DTA) inifiles/hybris-led.ini $(DESTDIR)$(CONFDIR)/20hybris-led.ini
	$(INSTALL_DTA) inifiles/hybris-features.ini $(DESTDIR)$(CONFDIR)/20hybris-features.ini
	$(INSTALL_DTA) inifiles/debug-led.ini $(DESTDIR)$(CONFDIR)/20debug-led.ini
	$(INSTALL_DTA) inifiles/als-defaults.ini $(DESTDIR)$(CONFDIR)/20als-defaults.ini
	$(INSTALL_DTA) inifiles/evdev-types.ini $(DESTDIR)$(CONFDIR)/20evdev-types.ini
	$(INSTALL_DTA) inifiles/legacy.ini $(DESTDIR)$(CONFDIR)/11legacy.ini

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

ifeq ($(ENABLE_UNITTESTS_INSTALL),y)
install:: install_unittests
endif

install_unittests::
	$(INSTALL_DIR) $(DESTDIR)$(TESTSDESTDIR)
	$(INSTALL_BIN) $(UTESTS) $(DESTDIR)$(TESTSDESTDIR)
	$(INSTALL_DTA) $(UTESTDIR)/tests.xml $(DESTDIR)$(TESTSDESTDIR)

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

# All sources, except ones from gmain git submodule
DEPEND_SOURCES = $(filter-out $(DBUS_GMAIN_DIR)/%.c, $(wildcard *.c */*.c */*/*.c))

.PHONY: depend
depend::
	@echo "Updating .depend"
	$(CC) -MM $(CPPFLAGS) $(MCE_CFLAGS) $(DEPEND_SOURCES) |\
	./depend_filter.py > .depend

ifneq ($(MAKECMDGOALS),depend) # not while: make depend
ifneq (,$(wildcard .depend))   # not if .depend does not exist
include .depend
endif
endif

# ----------------------------------------------------------------------------
# DEVELOPMENT TIME WHITESPACE NORMALIZE
# ----------------------------------------------------------------------------

NORMALIZE_USES_SPC =\
	bme-dbus-names.h\
	builtin-gconf.c\
	builtin-gconf.h\
	datapipe.c\
	datapipe.h\
	evdev.c\
	evdev.h\
	event-input.c\
	event-input.h\
	event-switches.h\
	filewatcher.c\
	filewatcher.h\
	homekey.dot\
	libwakelock.h\
	mce-common.c\
	mce-common.h\
	mce-dbus.h\
	mce-dsme.c\
	mce-dsme.h\
	mce-fbdev.c\
	mce-fbdev.h\
	mce-command-line.c\
	mce-command-line.h\
	mce-hbtimer.c\
	mce-hbtimer.h\
	mce-wltimer.c\
	mce-wltimer.h\
	mce-hybris.c\
	mce-hybris.h\
	mce-modules.h\
	mce-sensorfw.c\
	mce-sensorfw.h\
	mce-setting.h\
	mce-wakelock.c\
	mce-wakelock.h\
	mce-worker.c\
	mce-worker.h\
	modetransition.h\
	modules/alarm.c\
	modules/audiorouting.c\
	modules/battery-upower.c\
	modules/battery-statefs.c\
	modules/battery-udev.c\
	modules/bluetooth.c\
	modules/buttonbacklight.c\
	modules/callstate.c\
	modules/callstate.h\
	modules/camera.h\
	modules/cpu-keepalive.c\
	modules/display.c\
	modules/display.h\
	modules/display.dot\
	modules/doubletap.c\
	modules/doubletap.h\
	modules/filter-brightness-als.c\
	modules/filter-brightness-als.h\
	modules/fingerprint.c\
	modules/keypad.h\
	modules/inactivity.c\
	modules/inactivity.h\
	modules/memnotify.c\
	modules/memnotify.h\
	modules/mempressure.c\
	modules/packagekit.c\
	modules/powersavemode.h\
	modules/proximity.c\
	modules/proximity.h\
	modules/radiostates.h\
	modules/sensor-gestures.c\
	modules/usbmode.c\
	multitouch.c\
	multitouch.h\
	musl-compatibility.h\
	ofono-dbus-names.h\
	powerkey.c\
	powerkey.h\
	powerkey.dot\
	systemui/dbus-names.h\
	tklock.c\
	tklock.h\
	tools/evdev_trace.c\
	tools/dummy_compositor.c\
	tools/mcetool.c\
	tools/fileusers.c\
	tools/fileusers.h\

NORMALIZE_USES_TAB =\
	event-switches.c\
	libwakelock.c\
	mce-conf.c\
	mce-conf.h\
	mce-dbus.c\
	mce-setting.c\
	mce-hal.c\
	mce-hal.h\
	mce-io.c\
	mce-io.h\
	mce-lib.c\
	mce-lib.h\
	mce-log.c\
	mce-log.h\
	mce-modules.c\
	mce.c\
	mce.h\
	mce.conf\
	modetransition.c\
	modules/battery-bme.c\
	modules/camera.c\
	modules/keypad.c\
	modules/led.c\
	modules/led.h\
	modules/powersavemode.c\
	modules/radiostates.c\
	systemui/tklock-dbus-names.h\

NORMALIZE_KNOWN := $(NORMALIZE_USES_SPC) $(NORMALIZE_USES_TAB)
SOURCEFILES_ALL := $(wildcard *.[ch] modules/*.[ch])
NORMALIZE_UNKNOWN = $(filter-out $(NORMALIZE_KNOWN), $(SOURCEFILES_ALL))

.PHONY: normalize

normalize::
	normalize_whitespace -M Makefile
	normalize_whitespace -a inifiles/*.ini config/*.conf
	normalize_whitespace -t -b -e -s $(NORMALIZE_USES_SPC)
	normalize_whitespace -T -e -s $(NORMALIZE_USES_TAB)
ifneq ($(NORMALIZE_UNKNOWN),)
	@echo "Unknown source files: $(NORMALIZE_UNKNOWN)"
	false
endif

# ----------------------------------------------------------------------------
# DEVELOPMENT TIME PROTOTYPE SCANNING
# ----------------------------------------------------------------------------

.SUFFIXES: .q .p .g

PROTO_CPPFLAGS += $(CPPFLAGS)
PROTO_CPPFLAGS += $(MCE_CFLAGS)
PROTO_CPPFLAGS += $(MODULE_CFLAGS)
PROTO_CPPFLAGS += -D_Float32=float
PROTO_CPPFLAGS += -D_Float64=double
PROTO_CPPFLAGS += -D_Float128="long double"
PROTO_CPPFLAGS += -D_Float32x=float
PROTO_CPPFLAGS += -D_Float64x=double
PROTO_CPPFLAGS += -D_Float128x="long double"

%.q : %.c ; $(CC) -o $@ -E $< $(PROTO_CPPFLAGS)
%.p : %.q ; cproto -s < $< | prettyproto.py | tee $@
%.g : %.q ; cproto < $< | prettyproto.py | tee $@

protos-q: $(patsubst %.c,%.q,$(wildcard *.c modules/*.c))
protos-p: $(patsubst %.c,%.p,$(wildcard *.c modules/*.c))
protos-g: $(patsubst %.c,%.g,$(wildcard *.c modules/*.c))

clean::
	$(RM) -f *.[qpg] modules/*.[qpg] tools/*.[qpg]

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
