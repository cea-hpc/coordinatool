Name: coordinatool-phobos
Version: 0.1
Release: 1%{?dist}
Summary: lustre userspace coordinator implemented as a copytool
License: LGPLv3+

Source0: %{name}-%{version}.tar.xz

Requires: phobos >= 1.93
Requires: lustre-client

BuildRequires: meson
BuildRequires: gcc
BuildRequires: phobos >= 1.93
BuildRequires: lustre-client
BuildRequires: epel-rpm-macros
BuildRequires: phobos-devel >= 1.93
BuildRequires: pkgconfig(jansson)
BuildRequires: pkgconfig(liburcu)
BuildRequires: pkgconfig(glib-2.0)

%description
coordinatool-phobos is a lustre copytool that takes all requests off of lustre's
coordinator, then hand the requests back to normal copytools through a
LD_PRELOAD lib. It has been specialized to work with Phobos as a backend.

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
%{_bindir}/lhsmd_cdt_phobos
%{_unitdir}/coordinatool-phobos@.service
%config(noreplace) /etc/sysconfig/coordinatool-phobos

%files client
%{_bindir}/cdt-phobos-client

%files lib
%{_libdir}/libcdt_phobos_client.so

%post lib -p /sbin/ldconfig
%postun lib -p /sbin/ldconfig

%changelog
* Sun Jun 27 2021 Dominique Martinet <dominique.martinet@codewreck.org> - 0.1-1
- initial version

