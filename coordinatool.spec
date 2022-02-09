Name: coordinatool
Version: 0.1
Release: 1%{?dist}
Summary: lustre userspace coordinator implemented as a copytool
License: LGPLv3+

Source0: %{name}-%{version}.tar.xz

Requires: phobos
Requires: lustre-client

BuildRequires: meson
BuildRequires: gcc
BuildRequires: phobos-devel
BuildRequires: pkgconfig(jansson)
BuildRequires: pkgconfig(liburcu)
BuildRequires: pkgconfig(glib-2.0)

%description
coordinatool is a lustre copytool that takes all requests off lustre's
cooridnator, then hand the requests back to normal copytools through a
LD_PRELOAD lib

%package lib
Summary: coordinatool LD_PRELOAD lib

%description lib
LD_PRELOAD library for coordinatool

%package client
Summary: coordinatool standalone client

%description client
standalone coordinatool client to interact with the server

%prep
%autosetup

%build
%meson -Dwerror=false
%meson_build

%install
%meson_install

%check
%meson_test

%files
%{_bindir}/lhsmd_coordinatool
%{_unitdir}/coordinatool@.service
%config(noreplace) /etc/sysconfig/coordinatool

%files client
%{_bindir}/coordinatool-client

%files lib
%{_libdir}/libcoordinatool_client.so

%post lib -p /sbin/ldconfig
%postun lib -p /sbin/ldconfig

%changelog
* Sun Jun 27 2021 Dominique Martinet <dominique.martinet@codewreck.org> - 0.1-1
- initial version

