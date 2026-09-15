#!/usr/bin/env bash
#
# Builds the patched neatvnc RPM the way Fedora/EPEL does it: a pristine
# upstream source tarball plus a patch, assembled into an rpmbuild tree.
#
# The tarball is produced from the upstream release commit in this repository
# rather than downloaded, so the build is reproducible and needs no network.
# That commit is upstream's v0.9.6 tag content, which is what Source0 in the
# spec refers to.
#
# Usage: packaging/build-rpm.sh [output-directory]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${1:-${REPO_DIR}/rpms}"

# Upstream "Release v0.9.6". Override when rebasing onto a different release.
BASE_REF="${NEATVNC_BASE_REF:-3295c11}"

NAME=neatvnc
VERSION="$(sed -n "s/^\tversion: '\(.*\)',$/\1/p" "${REPO_DIR}/meson.build" | head -1)"
PATCH_NAME=0001-h264-add-NVENC-encoder-implementation.patch

# Everything the patch is allowed to touch. Packaging and CI live outside the
# tarball, so they must not end up in the patch. test/ is included so that the
# unit tests added alongside fixes actually run in the RPM's %check -- without
# it the rpmbuild tree holds upstream's tests only, and %check green-lights
# without ever executing the new ones.
PATCHED_PATHS=(src include test meson.build meson_options.txt)

SPEC_VERSION="$(sed -n 's/^Version:[[:space:]]*\(.*\)$/\1/p' \
	"${SCRIPT_DIR}/${NAME}.spec" | head -1)"

# The tarball is named from meson.build, but %prep unpacks the name built from
# the spec's Version. When those drift apart -- which is what happens when the
# branch is rebased onto a new upstream release -- rpmbuild fails deep inside
# %prep with a missing-file error that names neither cause. Say it plainly.
if [ "${VERSION}" != "${SPEC_VERSION}" ]; then
	echo "error: meson.build says ${VERSION}, ${NAME}.spec says ${SPEC_VERSION}." >&2
	echo "Update the spec's Version (and BASE_REF) for the new release." >&2
	exit 1
fi

echo "Building ${NAME}-${VERSION} (base ${BASE_REF})"

if ! git -C "${REPO_DIR}" rev-parse --verify --quiet "${BASE_REF}^{commit}" > /dev/null
then
	echo "error: base commit ${BASE_REF} is not in this checkout." >&2
	echo "Fetch the full history (git fetch --unshallow) and retry." >&2
	exit 1
fi

RPM_TOP="$(rpm --eval '%{_topdir}')"
mkdir -p "${RPM_TOP}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
mkdir -p "${OUT_DIR}"

git -C "${REPO_DIR}" archive --format=tar.gz \
	--prefix="${NAME}-${VERSION}/" \
	-o "${RPM_TOP}/SOURCES/${NAME}-${VERSION}.tar.gz" \
	"${BASE_REF}"

# A plain unified diff is all %autosetup -p1 needs.
git -C "${REPO_DIR}" diff "${BASE_REF}" -- "${PATCHED_PATHS[@]}" \
	> "${RPM_TOP}/SOURCES/${PATCH_NAME}"

if [ ! -s "${RPM_TOP}/SOURCES/${PATCH_NAME}" ]; then
	echo "error: the patch came out empty; is HEAD really ahead of ${BASE_REF}?" >&2
	exit 1
fi

echo "Patch is $(wc -l < "${RPM_TOP}/SOURCES/${PATCH_NAME}") lines"

cp "${SCRIPT_DIR}/${NAME}.spec" "${RPM_TOP}/SPECS/"

rpmbuild -ba "${RPM_TOP}/SPECS/${NAME}.spec"

find "${RPM_TOP}/RPMS" "${RPM_TOP}/SRPMS" -name '*.rpm' -exec cp -v {} "${OUT_DIR}/" \;

echo
echo "RPMs in ${OUT_DIR}:"
ls -1 "${OUT_DIR}"
