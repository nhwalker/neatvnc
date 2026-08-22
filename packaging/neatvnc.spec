%global forgeurl https://github.com/any1/neatvnc

Name:           neatvnc
Version:        0.9.1
# The .nvenc suffix marks this as the patched build. It sorts below a
# hypothetical future EPEL 0.9.1-1, so the runtime image should exclude
# neatvnc from EPEL -- see packaging/README.md.
Release:        1%{?dist}.nvenc
Summary:        A neat VNC server library

License:        ISC
URL:            %{forgeurl}
Source0:        %{forgeurl}/archive/v%{version}/%{name}-%{version}.tar.gz

# Adds an NVENC based H.264 encoder so that H.264 can be offloaded to an
# NVIDIA GPU, and lets the open-h264 encoding be used with frame buffers that
# live in main memory, which is what Weston's VNC backend produces.
Patch0:         0001-h264-add-NVENC-encoder-implementation.patch

BuildRequires:  gcc
BuildRequires:  meson >= 0.47
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(aml)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(gmp)
BuildRequires:  pkgconfig(gnutls)
BuildRequires:  pkgconfig(hogweed)
BuildRequires:  pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavfilter)
BuildRequires:  pkgconfig(libavutil)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libswscale)
BuildRequires:  pkgconfig(libturbojpeg)
BuildRequires:  pkgconfig(nettle)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(zlib)

%description
Neat VNC is a liberally licensed VNC server library that is easy to use and
powerful. It supports the raw, tight, zrle and open-h264 encodings, TLS and
RSA-AES authentication, and websocket transport.

This build additionally supports H.264 encoding on NVIDIA GPUs through NVENC.
Because NVENC accepts frame buffers from main memory, the open-h264 encoding
is available to compositors that do not hand out GBM buffer objects, such as
Weston's VNC backend. NVENC is used at runtime only if libnvidia-encode is
present; otherwise the library behaves exactly as it does upstream.

%package devel
Summary:        Development files for %{name}
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
The %{name}-devel package contains libraries and header files for developing
applications that use %{name}.

%prep
%autosetup -p1 -n %{name}-%{version}

%build
%meson -Dh264=enabled -Dnvenc=enabled -Dtests=true
%meson_build

%install
%meson_install

%check
%meson_test

%files
%license COPYING
%doc README.md
%{_libdir}/libneatvnc.so.0
%{_libdir}/libneatvnc.so.0.*

%files devel
%{_includedir}/neatvnc.h
%{_libdir}/libneatvnc.so
%{_libdir}/pkgconfig/neatvnc.pc

%changelog
* Sat Aug 22 2026 Nathan Walker <nathan.h.walker@gmail.com> - 0.9.1-1
- Initial package of neatvnc 0.9.1 with NVENC H.264 support
