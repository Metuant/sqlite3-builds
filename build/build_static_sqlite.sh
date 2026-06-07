#!/bin/sh
# Intentionally unquoted command fragments preserve optional sudo invocation.
# shellcheck disable=SC2086
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
pins_file="${script_dir}/../pins/versions.env"

if [ ! -r "${pins_file}" ]; then
  printf "FATAL: version pins file not readable: %s\n" "${pins_file}" >&2
  exit 1
fi

pin_default() {
  key="$1"
  . "${pins_file}"
  eval "printf '%s\n' \"\${${key}}\""
}

SQLITE_AMALG_URL="${SQLITE_AMALG_URL-$(pin_default SQLITE_AMALG_URL)}"
SQLITE_AMALG_SHA3_256="${SQLITE_AMALG_SHA3_256-$(pin_default SQLITE_AMALG_SHA3_256)}"
SQLITE_SRC_URL="${SQLITE_SRC_URL-$(pin_default SQLITE_SRC_URL)}"
SQLITE_SRC_SHA3_256="${SQLITE_SRC_SHA3_256-$(pin_default SQLITE_SRC_SHA3_256)}"
MIMALLOC_VERSION="${MIMALLOC_VERSION-$(pin_default MIMALLOC_VERSION)}"
MIMALLOC_URL="${MIMALLOC_URL-$(pin_default MIMALLOC_URL)}"
MIMALLOC_SHA512="${MIMALLOC_SHA512-$(pin_default MIMALLOC_SHA512)}"
ICU_VERSION="${ICU_VERSION-$(pin_default ICU_VERSION)}"
ICU_SHA512="${ICU_SHA512-$(pin_default ICU_SHA512)}"
LIBRARY_VARIANT="${LIBRARY_VARIANT:-generic}"
SQLite_compressor='upx'  # Program to use for compressing compiled sqlite
                         # Keep it empty as "" to disable compression

DockerCliImage='static-sqlite3-cli'
DockerLibraryImage='static-sqlite3-library'

### No user intervention below this point ######################################

onErr(){
  # Existing /bin/sh wrapper relies on shell-local support where it runs.
  # shellcheck disable=SC3043
  local msg errNum
  msg="${1}"
  errNum=$2
  printf "Error[%i]: %s\n" "${errNum}" "${msg}"
  exit 1
}

errMsgDep="Cannot continue due to absence of required dependency"

if ID=$(type id); then
  ID="/${ID#*/}"
else
  onErr "${errMsgDep}" 1
fi

if [ "$($ID -u)" -eq 0 ]; then
  SUDO=''
else
  if SUDO=$(type sudo); then
    SUDO="/${SUDO#*/}"
  else
    SUDO=''
  fi
fi

if DOCKER=$(type docker); then
  DOCKER="/${DOCKER#*/}"
else
  onErr "Please install docker first..." 2
fi

if [ "$(uname -s)" = "Linux" ]; then
  for _cmd in ldd file; do
    type "${_cmd}" >/dev/null 2>&1 || onErr "${errMsgDep}: ${_cmd}" 1
  done
fi

if ! $SUDO $DOCKER version  >/dev/null 2>&1; then
  errMsg="$(printf '\n  You are not authorized to run docker,')"
  errMsg="${errMsg}$(printf '\n  try to "su -" into root account and try again.\n\n')"
  onErr  "${errMsg}" 3
fi



$SUDO $DOCKER build --rm --no-cache=true -t "${DockerCliImage}"              \
  --build-arg SQLITE_AMALG_URL="${SQLITE_AMALG_URL}"                         \
  --build-arg COMPRESS_SQLITE3="${SQLite_compressor}"                        \
  --build-arg MARCH="${MARCH:-x86-64-v3}"                                    \
  --build-arg SQLITE_AMALG_SHA3_256="${SQLITE_AMALG_SHA3_256}"               \
  -f docker-cli/Dockerfile .

if [ "${LIBRARY_VARIANT}" = 'plex' ]; then
  $SUDO $DOCKER build --rm --no-cache=true -t "${DockerLibraryImage}"        \
    --build-arg SQLITE_AMALG_URL="${SQLITE_AMALG_URL}"                       \
    --build-arg MARCH="${MARCH:-x86-64-v3}"                                  \
    --build-arg LIBRARY_VARIANT="${LIBRARY_VARIANT}"                         \
    --build-arg SQLITE_AMALG_SHA3_256="${SQLITE_AMALG_SHA3_256}"             \
    --build-arg SQLITE_SRC_URL="${SQLITE_SRC_URL}"                           \
    --build-arg SQLITE_SRC_SHA3_256="${SQLITE_SRC_SHA3_256}"                 \
    --build-arg MIMALLOC_VERSION="${MIMALLOC_VERSION}"                       \
    --build-arg MIMALLOC_URL="${MIMALLOC_URL}"                               \
    --build-arg MIMALLOC_SHA512="${MIMALLOC_SHA512}"                         \
    --build-arg ICU_VERSION="${ICU_VERSION}"                                 \
    --build-arg ICU_SHA512="${ICU_SHA512}"                                   \
    -f docker-library/Dockerfile .
else
  $SUDO $DOCKER build --rm --no-cache=true -t "${DockerLibraryImage}"        \
    --build-arg SQLITE_AMALG_URL="${SQLITE_AMALG_URL}"                       \
    --build-arg MARCH="${MARCH:-x86-64-v3}"                                  \
    --build-arg LIBRARY_VARIANT="${LIBRARY_VARIANT}"                         \
    --build-arg SQLITE_AMALG_SHA3_256="${SQLITE_AMALG_SHA3_256}"             \
    --build-arg MIMALLOC_VERSION="${MIMALLOC_VERSION}"                       \
    --build-arg MIMALLOC_URL="${MIMALLOC_URL}"                               \
    --build-arg MIMALLOC_SHA512="${MIMALLOC_SHA512}"                         \
    -f docker-library/Dockerfile .
fi

printf "\n\n===== Taking ready to use static sqlite3 =======================================\n\n"

arch="${SQLITE_AMALG_URL##*/}"
workdir="${arch%.*}"

if [ "${LIBRARY_VARIANT}" = 'plex' ]; then
  # WHY: Plex output stays separate so the generic library artifact cannot be overwritten.
  library_output_dir='release/library-plex'
else
  library_output_dir='release/library'
fi

mkdir -p release release/cli "${library_output_dir}"

$SUDO $DOCKER run --rm -v "$(pwd)/release/cli:/release/"  \
  "${DockerCliImage}"                                     \
  cp -fv /app/${workdir}/dist/sqlite3 /app/${workdir}/dist/sqlite3_orig /release/

$SUDO $DOCKER run --rm -v "$(pwd)/${library_output_dir}:/release/"  \
  "${DockerLibraryImage}"                                     \
  cp -fv /app/${workdir}/dist/libsqlite3.so /release/

cd release/cli
echo "=============================="
ldd  sqlite3
echo "------------------------------"
file sqlite3
echo "------------------------------"
echo '.version' | ./sqlite3
echo "=============================="

cd "../${library_output_dir#release/}"
echo "=============================="
file libsqlite3.so
echo "------------------------------"
if [ "${LIBRARY_VARIANT}" = 'plex' ]; then
  printf 'ldd inspection skipped: Plex runtime ICU libraries are required on the host for plain ldd inspection of %s/libsqlite3.so\n' "${library_output_dir}"
else
  ldd  libsqlite3.so
fi
echo "=============================="

cd ../..

# Cleanup the built images
$SUDO $DOCKER image rm "${DockerCliImage}" "${DockerLibraryImage}" >/dev/null 2>&1
