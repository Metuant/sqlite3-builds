#!/bin/sh
set -eu

SQLITE_ZIP_URL='https://sqlite.org/2026/sqlite-amalgamation-3530100.zip'
SQLITE_SHA3_256='3c07136e4f6b5dd0c395be86455014039597bc65b6851f7111e88f71b6e06114'
SQLite_compressor='upx'  # Program to use for compressing compiled sqlite
                         # Keep it empty as "" to disable compression

DockerCliImage='static-sqlite3-cli'
DockerLibraryImage='static-sqlite3-library'

### No user intervention below this point ######################################

onErr(){
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

if ! $SUDO $DOCKER version  >/dev/null 2>&1; then
  errMsg="$(printf '\n  You are not authorized to run docker,')"
  errMsg="${errMsg}$(printf '\n  try to "su -" into root account and try again.\n\n')"
  onErr  "${errMsg}" 3
fi



$SUDO $DOCKER build --rm --no-cache=true -t "${DockerCliImage}"              \
  --build-arg URL_SQLITE_SOURCE_ZIP="${SQLITE_ZIP_URL}"                      \
  --build-arg COMPRESS_SQLITE3="${SQLite_compressor}"                        \
  --build-arg MARCH="${MARCH:-x86-64-v3}"                                    \
  --build-arg SQLITE_SHA3_256="${SQLITE_SHA3_256}"                           \
  -f docker-cli/Dockerfile .

$SUDO $DOCKER build --rm --no-cache=true -t "${DockerLibraryImage}"          \
  --build-arg URL_SQLITE_SOURCE_ZIP="${SQLITE_ZIP_URL}"                      \
  --build-arg MARCH="${MARCH:-x86-64-v3}"                                    \
  --build-arg SQLITE_SHA3_256="${SQLITE_SHA3_256}"                           \
  -f docker-library/Dockerfile .

printf "\n\n===== Taking ready to use static sqlite3 =======================================\n\n"

arch="${SQLITE_ZIP_URL##*/}"
workdir="${arch%.*}"

mkdir -p release release/cli release/library

$SUDO $DOCKER run --rm -v "$(pwd)/release/cli:/release/"  \
  "${DockerCliImage}"                                     \
  cp -fv /app/${workdir}/dist/sqlite3 /app/${workdir}/dist/sqlite3_orig /release/

$SUDO $DOCKER run --rm -v "$(pwd)/release/library:/release/"  \
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

cd ../library
echo "=============================="
file libsqlite3.so
echo "------------------------------"
ldd  libsqlite3.so
echo "=============================="

cd ../..

# Cleanup the built images
$SUDO $DOCKER image rm "${DockerCliImage}" "${DockerLibraryImage}" >/dev/null 2>&1
