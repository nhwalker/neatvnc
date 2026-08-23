#!/usr/bin/env bash
#
# Enables the repositories the patched neatvnc needs on top of a ubi10 image:
#
#   EPEL 10        aml, and (for the test image) weston, novnc, websockify
#   RPM Fusion     ffmpeg 7.x, which is the only EL10 build with --enable-nvenc
#   Rocky 10       mesa-libgbm, libdrm, turbojpeg, nettle and friends, which
#                  UBI itself does not ship
#
# UBI's own baseos/appstream/codeready-builder repos are enabled in the image.

set -euo pipefail

dnf -y install dnf-plugins-core
dnf -y install https://dl.fedoraproject.org/pub/epel/epel-release-latest-10.noarch.rpm
dnf -y install https://mirrors.rpmfusion.org/free/el/rpmfusion-free-release-10.noarch.rpm

cat > /etc/yum.repos.d/rocky10.repo <<'EOF'
[rocky-baseos]
name=Rocky Linux 10 - BaseOS
baseurl=https://dl.rockylinux.org/pub/rocky/10/BaseOS/$basearch/os/
gpgcheck=1
gpgkey=https://dl.rockylinux.org/pub/rocky/RPM-GPG-KEY-Rocky-10
enabled=1

[rocky-appstream]
name=Rocky Linux 10 - AppStream
baseurl=https://dl.rockylinux.org/pub/rocky/10/AppStream/$basearch/os/
gpgcheck=1
gpgkey=https://dl.rockylinux.org/pub/rocky/RPM-GPG-KEY-Rocky-10
enabled=1

[rocky-crb]
name=Rocky Linux 10 - CRB
baseurl=https://dl.rockylinux.org/pub/rocky/10/CRB/$basearch/os/
gpgcheck=1
gpgkey=https://dl.rockylinux.org/pub/rocky/RPM-GPG-KEY-Rocky-10
enabled=1
EOF

rpm --import https://dl.rockylinux.org/pub/rocky/RPM-GPG-KEY-Rocky-10

dnf -y makecache
