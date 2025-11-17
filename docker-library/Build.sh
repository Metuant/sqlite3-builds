#!/bin/sh

src="${1}"
arch="${src##*/}"
workdir="${arch%.*}"


if [ ! -f "${arch}" ]; then
  wget -c "${src}"
  [ $? -ne 0 ] && {
    echo "===== FAILED ....... ==========================================================="
    printf "Can not download: %s\n\n" "${src}"
    exit 1
  }
fi

[ ! -f "${workdir}/sqlite3.c" ] && unzip ${arch}

cd "${workdir}"
mkdir dist

printf "\n\n===== Compiling... Please wait... ==============================================\n\n"

gcc \
  -O3 \
  -march=x86-64-v3 \
  -mtune=generic \
  -flto \
  -fno-semantic-interposition \
  -DHAVE_USLEEP=1 \
  -DSQLITE_CORE \
  -DSQLITE_ALLOW_COVERING_INDEX_SCAN=1 \
  -DSQLITE_DEFAULT_CACHE_SIZE=-1048576 \
  -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
  -DSQLITE_DEFAULT_LOOKASIDE=2048,512 \
  -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_PAGE_SIZE=4096 \
  -DSQLITE_DEFAULT_MMAP_SIZE=8589934592 \
  -DSQLITE_DEFAULT_SYNCHRONOUS=2 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_DEFAULT_WORKER_THREADS=2 \
  -DSQLITE_DISABLE_DIRSYNC \
  -DSQLITE_DQS=0 \
  -DSQLITE_ENABLE_FTS5 \
  -DSQLITE_ENABLE_LOAD_EXTENSION \
  -DSQLITE_ENABLE_MATH_FUNCTIONS \
  -DSQLITE_ENABLE_MEMSYS5 \
  -DSQLITE_ENABLE_PREUPDATE_HOOK \
  -DSQLITE_ENABLE_RTREE \
  -DSQLITE_ENABLE_SORTER_REFERENCES \
  -DSQLITE_ENABLE_STAT4 \
  -DSQLITE_ENABLE_SESSION \
  -DSQLITE_ENABLE_UNLOCK_NOTIFY \
  -DSQLITE_ENABLE_UPDATE_DELETE_LIMIT \
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
  -DSQLITE_MAX_ATTACHED=125 \
  -DSQLITE_MAX_COLUMN=32767 \
  -DSQLITE_MAX_DEFAULT_PAGE_SIZE=32768 \
  -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_MAX_LENGTH=2147483647 \
  -DSQLITE_MAX_MMAP_SIZE=1099511627776 \
  -DSQLITE_MAX_PAGE_COUNT=4294967294 \
  -DSQLITE_MAX_PAGE_SIZE=65536 \
  -DSQLITE_MAX_SCHEMA_RETRY=25 \
  -DSQLITE_MAX_SQL_LENGTH=1073741824 \
  -DSQLITE_MAX_VARIABLE_NUMBER=250000 \
  -DSQLITE_MAX_WORKER_THREADS=8 \
  -DSQLITE_OMIT_DECLTYPE \
  -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_STMTJRNL_SPILL=-1 \
  -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_THREADSAFE=1 \
  -DSQLITE_USE_ALLOCA \
  -DSQLITE_USE_URI=1 \
  sqlite3.c \
  -fPIC \
  -shared \
  -lm \
  -lz \
  -ldl \
  -pthread \
  -o dist/libsqlite3.so

rc=$?
if [ $rc -ne 0 ]; then
  echo "================================ FAILED to compile ============================="
  exit 1
fi

echo "===================================== Done ====================================="

exit 0
