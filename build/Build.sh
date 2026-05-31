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
sqlite_amalg_sha3="${SQLITE_AMALG_SHA3_256}"
sqlite_src_sha3="${SQLITE_SRC_SHA3_256:-}"
LIBRARY_VARIANT="${LIBRARY_VARIANT:-generic}"
MARCH="${MARCH:-x86-64-v3}"
MIMALLOC_VERSION="${MIMALLOC_VERSION:-3.3.2}"
MIMALLOC_URL="${MIMALLOC_URL:-https://github.com/microsoft/mimalloc/archive/refs/tags/v3.3.2.tar.gz}"
MIMALLOC_SHA512="${MIMALLOC_SHA512:-226bbd51eca36d7737ce5e2edba7e0a3beeca448462a861bcbfb6726a0994bc077b4c684d7ff8b0805d71bf770e00df14f10ed598256ee54a154d8cc08e6a5c1}"
mimalloc_link_inputs=""

if [ "${LIBRARY_VARIANT}" = "plex" ] && [ -z "${SQLITE_SRC_URL:-}" ]; then
  echo 'plex variant requires SQLITE_SRC_URL'; exit 1
fi

case "${target}" in
  cli)
    target_cflags="-DHAVE_READLINE=1 -DSQLITE_HAVE_ZLIB -DSQLITE_ENABLE_BYTECODE_VTAB -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_EXPLAIN_COMMENTS -DSQLITE_ENABLE_STMTVTAB -DSQLITE_OMIT_AUTOINIT -DSQLITE_DEFAULT_PCACHE_INITSZ=128 -DSQLITE_DEFAULT_MEMSTATUS=1"
    sources="shell.c sqlite3.c"
    target_ldflags="-static -lm -lz -ldl -lreadline -lncursesw -pthread"
    output="dist/sqlite3"
    ;;
  library)
    target_cflags="-DSQLITE_ENABLE_NORMALIZE -DSQLITE_DISABLE_PAGECACHE_OVERFLOW_STATS -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_DEFAULT_PCACHE_INITSZ=256 -DSQLITE_DEFAULT_MEMSTATUS=0"
    sources="sqlite3.c /app/auto_extension.c /app/observability.c /app/slow_query_tracker.c"
    target_ldflags="-fPIC -shared -Wl,-z,defs -Wl,--version-script=/app/build/libsqlite3-version-script.ld -lm -ldl -pthread"
    output="dist/libsqlite3.so"
    MIMALLOC_OBJ="${MIMALLOC_OBJ:-/opt/mimalloc/lib/mimalloc.o}"
    MIMALLOC_LIB="${MIMALLOC_LIB:-/opt/mimalloc/lib/libmimalloc.a}"
    mimalloc_link_inputs="${MIMALLOC_OBJ} ${MIMALLOC_LIB}"
    case "${LIBRARY_VARIANT}" in
      generic)
        ;;
      plex)
        target_cflags="${target_cflags} -DSQLITE_ENABLE_ICU -DU_HAVE_LIB_SUFFIX=1 -DU_LIB_SUFFIX_C_NAME=_plex -I${PLEX_ICU_INCLUDE}"
        target_ldflags="${target_ldflags} -L${PLEX_ICU_LIB}"
        target_ldflags="${target_ldflags} -Wl,--push-state,--no-as-needed -licuucplex -licui18nplex -licudataplex -Wl,--pop-state"
        SQLITE_SRC_DIR="/app/$(basename "${SQLITE_SRC_URL%.zip}")"
        sources="${sources} ${SQLITE_SRC_DIR}/ext/icu/icu.c"
        ;;
      *)
        printf "Unknown LIBRARY_VARIANT: %s\n" "${LIBRARY_VARIANT}"
        exit 1
        ;;
    esac
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

  # WHY: SHA3 verification stays inline at each fetch site so each archive pin
  # stays next to the download it protects.
  if command -v sha3sum >/dev/null 2>&1; then
    if ! printf "%s  %s\n" "${sqlite_amalg_sha3}" "${arch}.tmp" | sha3sum -a 256 -c -; then
      echo "================================ FAILED source verification ===================="
      exit 1
    fi
  else
    actual_sha3="$(openssl dgst -sha3-256 "${arch}.tmp" | awk '{print $2}')"
    if [ "${actual_sha3}" != "${sqlite_amalg_sha3}" ]; then
      echo "================================ FAILED source verification ===================="
      exit 1
    fi
  fi

  mv "${arch}.tmp" "${arch}"
fi

if [ ! -f "${workdir}/sqlite3.c" ]; then
  unzip "${arch}"
fi

if [ "${LIBRARY_VARIANT}" = "plex" ]; then
  cd /app
  src_basename="$(basename "${SQLITE_SRC_URL}")"
  wget -q -O "${src_basename}" "${SQLITE_SRC_URL}"
  if command -v sha3sum >/dev/null 2>&1; then
    if ! printf "%s  %s\n" "${sqlite_src_sha3}" "${src_basename}" | sha3sum -a 256 -c -; then
      echo "================================ FAILED source verification ===================="
      exit 1
    fi
  else
    actual_sha3="$(openssl dgst -sha3-256 "${src_basename}" | awk '{print $2}')"
    if [ "${actual_sha3}" != "${sqlite_src_sha3}" ]; then
      echo "================================ FAILED source verification ===================="
      exit 1
    fi
  fi
  unzip -q "${src_basename}"
  cd "${workdir}"
else
  cd "${workdir}"
fi
mkdir -p dist

if [ "${target}" = "library" ]; then
  if [ -z "${MIMALLOC_VERSION}" ] || [ -z "${MIMALLOC_URL}" ] || [ -z "${MIMALLOC_SHA512}" ]; then
    printf "FATAL: MIMALLOC_VERSION, MIMALLOC_URL, and MIMALLOC_SHA512 are required for library builds\n" >&2
    exit 1
  fi
  if [ ! -f /opt/mimalloc/SHA512 ]; then
    printf "FATAL: mimalloc SHA512 sentinel not found at /opt/mimalloc/SHA512; ensure the Dockerfile mimalloc stage ran\n" >&2
    exit 1
  fi
  if [ "$(cat /opt/mimalloc/SHA512)" != "${MIMALLOC_SHA512}" ]; then
    printf "FATAL: mimalloc SHA512 sentinel drift at /opt/mimalloc/SHA512; ensure the Dockerfile mimalloc stage used the current MIMALLOC_SHA512\n" >&2
    exit 1
  fi
  if [ ! -f "${MIMALLOC_OBJ}" ]; then
    printf "FATAL: mimalloc object not found at %s; ensure the Dockerfile mimalloc stage ran\n" "${MIMALLOC_OBJ}" >&2
    exit 1
  fi
  if [ ! -f "${MIMALLOC_LIB}" ]; then
    printf "FATAL: mimalloc static library not found at %s; ensure the Dockerfile mimalloc stage ran\n" "${MIMALLOC_LIB}" >&2
    exit 1
  fi

  patch -p1 < /app/build/sqlite-amalgamation.patch
fi

printf "\n\n===== Compiling... Please wait... ==============================================\n\n"

# SQLITE_DEFAULT_MMAP_SIZE=34359738368 is 32 GiB; aggressive for low-memory hosts (raises VSZ).
# SQLITE_ENABLE_UPDATE_DELETE_LIMIT is inactive on amalgamation builds (requires Lemon parser regen).
# SQLITE_MAX_EXPR_DEPTH=0 disables runtime depth checks; relies on MAX_LENGTH/MAX_SQL_LENGTH for DoS protection.
# SQLITE_THREADSAFE=2 is Multi-thread (no per-connection mutex); PMS and Emby call sqlite3_config(SQLITE_CONFIG_MULTITHREAD) at startup, Emby calls it twice per startup and also passes SQLITE_OPEN_NOMUTEX -- neither shares sqlite3* handles across threads.
# Build flag groups are space-delimited argument vectors.
# shellcheck disable=SC2086
if ! ${CC:-gcc} \
  -O3 \
  -march="${MARCH}" \
  -mtune=generic \
  -flto \
  -fno-semantic-interposition \
  -I. \
  -DHAVE_FDATASYNC=1 \
  -DHAVE_MALLOC_USABLE_SIZE=1 \
  -DHAVE_POSIX_FALLOCATE=1 \
  -DHAVE_STRCHRNUL=1 \
  -DHAVE_USLEEP=1 \
  -DSQLITE_CORE \
  -DSQLITE_ALLOW_COVERING_INDEX_SCAN=1 \
  -DSQLITE_DEFAULT_CACHE_SIZE=-1048576 \
  -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
  -DSQLITE_DEFAULT_LOOKASIDE=2048,512 \
  -DSQLITE_DEFAULT_PAGE_SIZE=16384 \
  -DSQLITE_DEFAULT_MMAP_SIZE=34359738368 \
  -DSQLITE_DEFAULT_SYNCHRONOUS=2 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_DEFAULT_WAL_AUTOCHECKPOINT=16000 \
  -DSQLITE_DEFAULT_WORKER_THREADS=8 \
  -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=67108864 \
  -DSQLITE_DIRECT_OVERFLOW_READ \
  -DSQLITE_DISABLE_DIRSYNC \
  -DSQLITE_DQS=1 \
  -DSQLITE_ENABLE_CARRAY \
  -DSQLITE_ENABLE_COLUMN_METADATA \
  -DSQLITE_ENABLE_FTS3 \
  -DSQLITE_ENABLE_FTS3_PARENTHESIS \
  -DSQLITE_ENABLE_FTS3_TOKENIZER \
  -DSQLITE_ENABLE_FTS5 \
  -DSQLITE_ENABLE_LOAD_EXTENSION \
  -DSQLITE_ENABLE_MATH_FUNCTIONS \
  -DSQLITE_ENABLE_NULL_TRIM \
  -DSQLITE_ENABLE_PERCENTILE \
  -DSQLITE_ENABLE_PREUPDATE_HOOK \
  -DSQLITE_ENABLE_RTREE \
  -DSQLITE_ENABLE_SESSION \
  -DSQLITE_ENABLE_SETLK_TIMEOUT \
  -DSQLITE_ENABLE_SORTER_REFERENCES \
  -DSQLITE_ENABLE_STAT4 \
  -DSQLITE_ENABLE_UPDATE_DELETE_LIMIT \
  -DSQLITE_HAVE_ISNAN \
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
  -DSQLITE_SORTER_PMASZ=8192 \
  -DSQLITE_STMTJRNL_SPILL=-1 \
  -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_THREADSAFE=2 \
  -DSQLITE_USE_ALLOCA \
  -DSQLITE_USE_URI=1 \
  ${target_cflags} \
  ${mimalloc_link_inputs} \
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

if [ "${target}" = "library" ]; then
  undefined_real_symbols="$(nm -D --undefined-only "${output}" | awk '$1 == "U" && $2 ~ /^sqlite3_.*_real$/ { print }')"
  if [ -n "${undefined_real_symbols}" ]; then
    printf "FATAL: unresolved sqlite3_*_real symbols in %s:\n%s\n" "${output}" "${undefined_real_symbols}" >&2
    exit 1
  fi
  exported_real_symbols="$(readelf --dyn-syms -W "${output}" | awk '$5 ~ /^(GLOBAL|WEAK)$/ && $7 != "UND" && $8 ~ /^sqlite3_.*_real$/ { print }')"
  if [ -n "${exported_real_symbols}" ]; then
    printf "FATAL: exported sqlite3_*_real symbols in %s:\n%s\n" "${output}" "${exported_real_symbols}" >&2
    exit 1
  fi
  exported_lazy_helper="$(readelf --dyn-syms -W "${output}" | awk '$5 ~ /^(GLOBAL|WEAK)$/ && $7 != "UND" && $8 == "auto_extension_register_for_open" { print }')"
  if [ -n "${exported_lazy_helper}" ]; then
    printf "FATAL: exported auto_extension_register_for_open symbol in %s:\n%s\n" "${output}" "${exported_lazy_helper}" >&2
    exit 1
  fi
  if readelf --dyn-syms -W "${output}" | awk '$5 ~ /^(GLOBAL|WEAK)$/ && $7 != "UND" && $8 == "auto_extension_register_for_open" { found=1 } END { exit found ? 0 : 1 }'; then
    printf "FATAL: auto_extension_register_for_open present in dynamic symbol table for %s\n" "${output}" >&2
    exit 1
  fi
  leaked_allocator_symbols="$(readelf --dyn-syms -W "${output}" | awk '$5 ~ /^(GLOBAL|WEAK)$/ && $7 != "UND" && ($8 ~ /^(mi_|_mi_|mimalloc)/ || $8 ~ /^(malloc|calloc|realloc|free|aligned_alloc|posix_memalign)$/) { print }')"
  if [ -n "${leaked_allocator_symbols}" ]; then
    printf "FATAL: leaked allocator symbol in %s:\n%s\n" "${output}" "${leaked_allocator_symbols}" >&2
    exit 1
  fi
  if ! nm --defined-only "${output}" | awk '$NF ~ /^_mi_/ || $NF ~ /^mi_/ || $NF == "malloc" { found=1 } END { exit found ? 0 : 1 }'; then
    printf "FATAL: no local mimalloc implementation symbols found in %s\n" "${output}" >&2
    exit 1
  fi

  missing_obs_entry=0
  config_names="$(grep -E '^#define SQLITE_CONFIG_[A-Z0-9_]+[[:space:]]+[0-9]+' sqlite3.h | awk '$2 !~ /_MAX$/ {print $2}')"
  config_count="$(printf "%s\n" "${config_names}" | awk 'NF { count++ } END { print count + 0 }')"
  if [ ! -f /app/build/expected-sqlite-config-count.txt ]; then
    printf "FATAL: /app/build/expected-sqlite-config-count.txt not found (regenerate from sqlite3.h #define SQLITE_CONFIG_* count if SQLite was bumped)\n" >&2
    exit 1
  fi
  expected_config_count="$(tr -d '[:space:]' < /app/build/expected-sqlite-config-count.txt)"
  case "${expected_config_count}" in
    ''|*[!0-9]*)
      printf "FATAL: /app/build/expected-sqlite-config-count.txt contains non-numeric content: '%s' (regenerate from sqlite3.h #define SQLITE_CONFIG_* count)\n" "${expected_config_count}" >&2
      exit 1
      ;;
  esac
  if [ "${config_count}" -ne "${expected_config_count}" ]; then
    printf "FATAL: extracted %s SQLITE_CONFIG_* names from sqlite3.h; expected exactly %s (regenerate build/expected-sqlite-config-count.txt if SQLite was bumped)\n" \
      "${config_count}" "${expected_config_count}" >&2
    exit 1
  fi
  for name in ${config_names}; do
    if ! grep -Eq "\\{[[:space:]]*${name}," /app/observability.c; then
      printf "FATAL: missing observability config decode entry for %s\n" "${name}" >&2
      missing_obs_entry=1
    fi
    if ! grep -Eq "case[[:space:]]+${name}:" /app/observability.c; then
      printf "FATAL: missing observability config forwarding case for %s\n" "${name}" >&2
      missing_obs_entry=1
    fi
  done
  dbconfig_names="$(grep -E '^#define SQLITE_DBCONFIG_[A-Z0-9_]+[[:space:]]+[0-9]+' sqlite3.h | awk '$2 != "SQLITE_DBCONFIG_MAX" {print $2}')"
  dbconfig_count="$(printf "%s\n" "${dbconfig_names}" | awk 'NF { count++ } END { print count + 0 }')"
  if [ ! -f /app/build/expected-sqlite-dbconfig-count.txt ]; then
    printf "FATAL: /app/build/expected-sqlite-dbconfig-count.txt not found (regenerate from sqlite3.h #define SQLITE_DBCONFIG_* count if SQLite was bumped)\n" >&2
    exit 1
  fi
  expected_dbconfig_count="$(tr -d '[:space:]' < /app/build/expected-sqlite-dbconfig-count.txt)"
  case "${expected_dbconfig_count}" in
    ''|*[!0-9]*)
      printf "FATAL: /app/build/expected-sqlite-dbconfig-count.txt contains non-numeric content: '%s' (regenerate from sqlite3.h #define SQLITE_DBCONFIG_* count)\n" "${expected_dbconfig_count}" >&2
      exit 1
      ;;
  esac
  if [ "${dbconfig_count}" -ne "${expected_dbconfig_count}" ]; then
    printf "FATAL: extracted %s SQLITE_DBCONFIG_* names from sqlite3.h; expected exactly %s (regenerate build/expected-sqlite-dbconfig-count.txt if SQLite was bumped)\n" \
      "${dbconfig_count}" "${expected_dbconfig_count}" >&2
    exit 1
  fi
  for name in ${dbconfig_names}; do
    [ "$name" = "SQLITE_DBCONFIG_MAX" ] && continue
    if ! grep -Eq "\\{[[:space:]]*${name}," /app/observability.c; then
      printf "FATAL: missing observability db-config decode entry for %s\n" "${name}" >&2
      missing_obs_entry=1
    fi
    if ! grep -Eq "case[[:space:]]+${name}:" /app/observability.c; then
      printf "FATAL: missing observability db-config forwarding case for %s\n" "${name}" >&2
      missing_obs_entry=1
    fi
  done
  if [ "${missing_obs_entry}" -ne 0 ]; then
    exit 1
  fi
fi

echo "===================================== Done ====================================="

exit 0
