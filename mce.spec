Name:     mce
Version:  1.10.92
Release:  2
Summary:  Mode Control Entity for Nokia mobile computers
Group:    System/System Control
License:  LGPLv2
URL:      http://meego.gitorious.org/meego-middleware/mce/
Source0:  %{name}-%{version}.tar.bz2
Patch0:   %{name}-1.10.90-no-ownership.patch
Patch1:   %{name}-1.10.90-include-i2c-fix.patch
Patch2:   %{name}-1.10.90-no-werror.patch

BuildRequires: pkgconfig(dbus-1) >= 1.0.2
BuildRequires: pkgconfig(dbus-glib-1)
BuildRequires: pkgconfig(dsme) >= 0.58
BuildRequires: pkgconfig(gconf-2.0)
BuildRequires: pkgconfig(glib-2.0) >= 2.18.0
BuildRequires: pkgconfig(mce) >= 1.10.21
BuildRequires: kernel-headers >= 2.6.32
BuildRequires: libi2c-devel

%description
This package contains the Mode Control Entity which provides
mode management features.  This is a daemon that is the backend
for many features on Nokia's mobile computers.

%package tools
Summary:  Tools for interacting with mce
Group:    Development/Tools
Requires: %{name} = %{version}-%{release}

%description tools
This package contains tools that can be used to interact with
the Mode Control Entity and to get mode information.

%prep
%setup -q
%patch0 -p1
%patch1 -p1
%patch2 -p1

%build
make %{?_smp_mflags}

%install
%make_install
install -d %{buildroot}/%{_mandir}/man8
install -m 644 man/mce.8 %{buildroot}/%{_mandir}/man8/mce.8
install -m 644 man/mcetool.8 %{buildroot}/%{_mandir}/man8/mcetool.8
install -m 644 man/mcetorture.8 %{buildroot}/%{_mandir}/man8/mcetorture.8
install -d %{buildroot}/%{_mandir}/sv/man8
install -m 644 man/mce.sv.8 %{buildroot}/%{_mandir}/sv/man8/mce.8
install -m 644 man/mcetool.sv.8 %{buildroot}/%{_mandir}/sv/man8/mcetool.8
install -m 644 man/mcetorture.sv.8 %{buildroot}/%{_mandir}/sv/man8/mcetorture.8

%files
%defattr(-,root,root,-)
%doc COPYING debian/changelog debian/copyright TODO
%config(noreplace) %{_sysconfdir}/dbus-1/system.d/mce.conf
%config(noreplace) %{_sysconfdir}/gconf/schemas/*.schemas
%config(noreplace) %{_sysconfdir}/%{name}/mce.ini
/sbin/%{name}
%{_libdir}/%{name}/modules/*.so
%{_sharedstatedir}/%{name}/radio_states.*
%{_datadir}/backup-framework/applications/mcebackup.conf
%{_mandir}/man8/%{name}.8.gz
%{_mandir}/sv/man8/%{name}.8.gz
%{_datadir}/mce/mce-*

%files tools
%defattr(-,root,root,-)
%doc COPYING debian/copyright
%config(noreplace) %{_sysconfdir}/dbus-1/system.d/mcetool.conf
/sbin/mcetool
/sbin/mcetorture
%{_mandir}/man8/mcetool.8.gz
%{_mandir}/sv/man8/mcetool.8.gz
%{_mandir}/man8/mcetorture.8.gz
%{_mandir}/sv/man8/mcetorture.8.gz
