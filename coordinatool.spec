Name: coordinatool
Version: 0.7
Release: 1%{?dist}
Summary: lustre userspace coordinator implemented as a copytool
License: LGPLv3+

Source0: %{name}-%{version}.tar.xz

%bcond_without phobos

%if %{with phobos}
Conflicts: coordinatool-phobos
%endif

BuildRequires: meson
BuildRequires: gcc
BuildRequires: pkgconfig(jansson)
BuildRequires: pkgconfig(liburcu)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: hiredis-devel
BuildRequires: pkgconfig(libsystemd)
BuildRequires: (lustre-client < 2.15 or lustre-devel)
BuildRequires: hiredis

Requires: lustre-client
Requires: hiredis
Requires: jansson
Requires: userspace-rcu

%description
coordinatool is a lustre copytool that takes all requests off lustre's
coordinator, then hand the requests back to normal copytools through a
LD_PRELOAD lib

%package lib
Summary: coordinatool LD_PRELOAD lib

%description lib
LD_PRELOAD library for coordinatool

%package client
Summary: coordinatool standalone client

%description client
standalone coordinatool client to interact with the server

%if %{with phobos}
%package phobos
Summary: lustre userspace coordinator for Phobos
Conflicts: coordinatool

BuildRequires: meson
BuildRequires: gcc
BuildRequires: pkgconfig(jansson)
BuildRequires: pkgconfig(liburcu)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: hiredis-devel
BuildRequires: (lustre-client < 2.15 or lustre-devel)
BuildRequires: hiredis
BuildRequires: phobos-devel >= 3

Requires: lustre-client
Requires: hiredis
Requires: jansson
Requires: userspace-rcu
Requires: phobos >= 3

%description phobos
lustre userspace coordinator implemented as a copytool specialized for Phobos
%endif

%prep
%autosetup

%build
RECONFIGURE=""
CONFIG_OPTS=""

%if %{with phobos}
CONFIG_OPTS="$CONFIG_OPTS -Dphobos=enabled"
RECONFIGURE="--reconfigure"

%meson -Dwerror=false $CONFIG_OPTS
%meson_build
mv %_vpath_builddir/lhsmd_coordinatool %_vpath_builddir/lhsmd_coordinatool_phobos
cp %_vpath_builddir/coordinatool@.service %_vpath_builddir/coordinatool-phobos@.service
sed -i 's/lhsmd_coordinatool/lhsmd_coordinatool_phobos/' %_vpath_builddir/coordinatool-phobos@.service
%endif

%meson -Dwerror=false -Dphobos=disabled $RECONFIGURE
%meson_build

%install
%meson_install

%if %{with phobos}
install_dir=../../BUILDROOT/%{NAME}-%{VERSION}-%{RELEASE}.x86_64
cp %_vpath_builddir/lhsmd_coordinatool_phobos $install_dir/%{_bindir}
cp %_vpath_builddir/coordinatool-phobos@.service $install_dir/%{_unitdir}
%endif

%files
%{_bindir}/lhsmd_coordinatool
%{_unitdir}/coordinatool@.service
%config(noreplace) /etc/sysconfig/coordinatool
%config(noreplace) %{_sysconfdir}/coordinatool.conf

%files client
%{_bindir}/coordinatool-client

%files lib
%{_libdir}/libcoordinatool_client.so

%if %{with phobos}
%files phobos
%{_bindir}/lhsmd_coordinatool_phobos
%{_unitdir}/coordinatool-phobos@.service
%config(noreplace) /etc/sysconfig/coordinatool
%config(noreplace) %{_sysconfdir}/coordinatool.conf
%endif

%post lib -p /sbin/ldconfig
%postun lib -p /sbin/ldconfig

%changelog
* Thu Oct 22 2026 Yoann Valeri <yoann.valeri@cea.fr> - 0.7-1
- Add a fuid-xattr parameter to the conf to set the xattr to get for the Phobos
  OID

* Thu Jul 9 2026 Gauthier Evraerd <gauthier.evraerd@cea.fr> - 0.6-1
- Return an error when the parsing of the configuration fails
- Notify systemd if the daemon fails to start

* Thu Feb 26 2026 Gauthier Evraerd <gauthier.evraerd@cea.fr> - 0.5-1
- Install the configuration file
- Add a specialized version of archive_on_host with a consistent hash. Also
  allow replacing the tag value with hash % N.
- Add cancellation of HSM requests
- Fix the tests

* Thu Jun 12 2025 Patrice Lucas <patrice.lucas@cea.fr> - 0.4-2
- Update to new Phobos 3 API (phobos_locate has a new "copy-name" argument)
- archive batching
- restore reporting

* Tue Feb 11 2025 Guillaume Courrier <guillaume.courrier@cea.fr> - 0.4-1
- Add support for archive_on_host to select which copytool gets a request based
  on HSM hints;
- Add support for request batches. Batch requests based on hints to store them
  on the same tape using Phobos.

* Wed Oct 9 2024 Guillaume Courrier <guillaume.courrier@cea.fr> - 0.3-2
- Add 'archive_on_host' config item to direct tags to copytools

* Fri Oct 20 2023 Patrice Lucas <patrice.lucas@cea.fr> - 0.3-1
- Update to new Phobos 1.95 API (phobos_init and phobos_locate)

* Tue Oct 25 2022 Guillaume Courrier <guillaume.courrier@cea.fr> - 0.2-1
- Clients will try to reconnect the server when down
- Use a REDIS database to store requests and recover from crash
- Add compile time switch to use Phobos features

* Sun Jun 27 2021 Dominique Martinet <dominique.martinet@codewreck.org> - 0.1-1
- initial version

