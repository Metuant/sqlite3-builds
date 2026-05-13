#!/bin/sh
set -eu

target="${1}"
case "${target}" in
  --target=*) target="${target#--target=}" ;;
esac

src="${2}"
compressor="${3:-}"
arch="${src##*/}"
workdir="${arch%.*}"
sqlite_sha3="${SQLITE_SHA3_256}"
MARCH="${MARCH:-x86-64-v3}"

case "${target}" in
  cli)
    target_cflags="-DHAVE_READLINE=1 -DSQLITE_HAVE_ZLIB -DSQLITE_ENABLE_BYTECODE_VTAB -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_EXPLAIN_COMMENTS -DSQLITE_ENABLE_STMTVTAB -DSQLITE_OMIT_AUTOINIT -DSQLITE_DEFAULT_PCACHE_INITSZ=128 -DSQLITE_DEFAULT_MEMSTATUS=1"
    sources="shell.c sqlite3.c"
    target_ldflags="-static -lm -lz -ldl -lreadline -lncursesw -pthread"
    output="dist/sqlite3"
    ;;
  library)
    target_cflags="-DSQLITE_ENABLE_NORMALIZE -DSQLITE_DISABLE_PAGECACHE_OVERFLOW_STATS -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_DEFAULT_PCACHE_INITSZ=256 -DSQLITE_DEFAULT_MEMSTATUS=0"
    sources="sqlite3.c"
    target_ldflags="-fPIC -shared -lm -ldl -pthread"
    output="dist/libsqlite3.so"
    ;;
  *)
    printf "Unknown target: %s\n" "${target}"
    exit 1
    ;;
esac

if [ ! -f "${arch}" ]; then
  if ! wget -O "${arch}.tmp" "${src}"; then
    echo "===== FAILED ....... ==========================================================="
    printf "Can not download: %s\n\n" "${src}"
    exit 1
  fi

  if command -v sha3sum >/dev/null 2>&1; then
    if ! printf "%s  %s\n" "${sqlite_sha3}" "${arch}.tmp" | sha3sum -a 256 -c -; then
      echo "================================ FAILED source verification ===================="
      exit 1
    fi
  else
    actual_sha3="$(openssl dgst -sha3-256 "${arch}.tmp" | awk '{print $2}')"
    if [ "${actual_sha3}" != "${sqlite_sha3}" ]; then
      echo "================================ FAILED source verification ===================="
      exit 1
    fi
  fi

  mv "${arch}.tmp" "${arch}"
fi

if [ ! -f "${workdir}/sqlite3.c" ]; then
  unzip "${arch}"
fi

cd "${workdir}"
mkdir -p dist

printf "\n\n===== Compiling... Please wait... ==============================================\n\n"

# SQLITE_DEFAULT_MMAP_SIZE=8589934592 is 8 GiB; aggressive for low-memory hosts (raises VSZ).
# SQLITE_ENABLE_UPDATE_DELETE_LIMIT is inactive on amalgamation builds (requires Lemon parser regen).
# SQLITE_MAX_EXPR_DEPTH=0 disables runtime depth checks; relies on MAX_LENGTH/MAX_SQL_LENGTH for DoS protection.
if ! ${CC:-gcc} \
  -O3 \
  -march="${MARCH}" \
  -mtune=generic \
  -flto \
  -fno-semantic-interposition \
  -DHAVE_FDATASYNC=1 \
  -DHAVE_MALLOC_USABLE_SIZE=1 \
  -DHAVE_STRCHRNUL=1 \
  -DHAVE_USLEEP=1 \
  -DSQLITE_CORE \
  -DSQLITE_ALLOW_COVERING_INDEX_SCAN=1 \
  -DSQLITE_DEFAULT_CACHE_SIZE=-1048576 \
  -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
  -DSQLITE_DEFAULT_LOOKASIDE=2048,512 \
  -DSQLITE_DEFAULT_PAGE_SIZE=4096 \
  -DSQLITE_DEFAULT_MMAP_SIZE=8589934592 \
  -DSQLITE_DEFAULT_SYNCHRONOUS=2 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_DEFAULT_WAL_AUTOCHECKPOINT=16000 \
  -DSQLITE_DEFAULT_WORKER_THREADS=2 \
  -DSQLITE_DISABLE_DIRSYNC \
  -DSQLITE_DQS=0 \
  -DSQLITE_ENABLE_CARRAY \
  -DSQLITE_ENABLE_COLUMN_METADATA \
  -DSQLITE_ENABLE_FTS5 \
  -DSQLITE_ENABLE_LOAD_EXTENSION \
  -DSQLITE_ENABLE_MATH_FUNCTIONS \
  -DSQLITE_ENABLE_NULL_TRIM \
  -DSQLITE_ENABLE_PERCENTILE \
  -DSQLITE_ENABLE_PREUPDATE_HOOK \
  -DSQLITE_ENABLE_RTREE \
  -DSQLITE_ENABLE_SORTER_REFERENCES \
  -DSQLITE_ENABLE_STAT4 \
  -DSQLITE_ENABLE_SESSION \
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
  -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_SORTER_PMASZ=1024 \
  -DSQLITE_STMTJRNL_SPILL=-1 \
  -DSQLITE_STRICT_SUBTYPE=1 \
  -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_THREADSAFE=1 \
  -DSQLITE_USE_ALLOCA \
  -DSQLITE_USE_URI=1 \
  ${target_cflags} \
  ${sources} \
  ${target_ldflags} \
  -o "${output}"; then
  echo "================================ FAILED to compile ============================="
  exit 1
fi

if [ "${target}" = "cli" ]; then
  cp dist/sqlite3 dist/sqlite3_orig   # Keep naked, unstripped, unpacked version
  echo "===== Stripping... ============================================================="
  strip --strip-unneeded dist/sqlite3
  if [ -n "${compressor}" ]; then
    echo "===== Compressing... ==========================================================="
    ${compressor} dist/sqlite3
  fi
fi

echo "===================================== Done ====================================="

exit 0
