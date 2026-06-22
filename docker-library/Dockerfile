# syntax=docker/dockerfile:1.5
ARG LIBRARY_VARIANT=generic

FROM ghcr.io/linuxserver/baseimage-ubuntu:noble AS base-generic

SHELL ["/bin/bash", "-c"]

RUN echo "**** Install Dependencies ****" && \
    apt-get -y update && \
    apt-get -y upgrade && \
    apt-get -y install --no-install-recommends \
        apt-transport-https \
        ca-certificates \
        openssl \
        software-properties-common \
        file \
        gpg-agent \
        wget \
        unzip && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/ppa && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/test && \
    apt-get -y upgrade && \
    apt-get -y install --no-install-recommends \
        cmake \
        make \
        g++ \
        gcc \
        binutils \
        patch \
        zlib1g \
        zlib1g-dev

FROM ghcr.io/linuxserver/baseimage-alpine:3.23 AS base-plex

RUN echo "**** Install Dependencies ****" && \
    apk add --no-cache \
        autoconf \
        bash \
        binutils \
        ca-certificates \
        cmake \
        coreutils \
        file \
        g++ \
        gcc \
        linux-headers \
        m4 \
        make \
        musl-dev \
        openssl \
        patch \
        pkgconf \
        python3 \
        unzip \
        wget

FROM base-${LIBRARY_VARIANT} AS build

LABEL org.sqlite.static.library.linux.x86_64.dockerfile.version="2.0.0"

SHELL ["/bin/bash", "-c"]

ENTRYPOINT []
CMD ["/bin/sh"]

ARG LIBRARY_VARIANT=generic

WORKDIR /app

COPY build/Build.sh /app/Build.sh
COPY src/auto_extension.c /app/auto_extension.c
COPY src/observability.c /app/observability.c
COPY src/slow_query_tracker.c /app/slow_query_tracker.c
COPY build/sqlite-amalgamation.patch /app/build/sqlite-amalgamation.patch
COPY build/expected-sqlite-config-count.txt /app/build/expected-sqlite-config-count.txt
COPY build/expected-sqlite-dbconfig-count.txt /app/build/expected-sqlite-dbconfig-count.txt
COPY build/libsqlite3-version-script.ld /app/build/libsqlite3-version-script.ld
COPY tests/ /app/tests/

RUN chmod +x /app/Build.sh

ARG ICU_SOURCE_VERSION
ARG ICU_SOURCE_SHA512
RUN if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      set -euo pipefail; \
      icu_release_version="$(printf '%s' "${ICU_SOURCE_VERSION}" | tr '.' '-')"; \
      icu_archive_version="$(printf '%s' "${ICU_SOURCE_VERSION}" | tr '.' '_')"; \
      icu_archive="/tmp/icu4c-${icu_archive_version}-src.tgz"; \
      icu_url="https://github.com/unicode-org/icu/releases/download/release-${icu_release_version}/icu4c-${icu_archive_version}-src.tgz"; \
      wget -O "${icu_archive}" "${icu_url}"; \
      echo "${ICU_SOURCE_SHA512}  ${icu_archive}" | sha512sum -c -; \
      tar -xzf "${icu_archive}" -C /tmp; \
      cd /tmp/icu/source; \
      ./configure \
        --prefix=/opt/icu-69-plex \
        --with-library-suffix=plex \
        --with-data-packaging=library \
        --enable-shared --disable-static \
        --disable-extras --disable-tests --disable-samples; \
      make -j$(nproc) && make install; \
    fi

ENV PLEX_ICU_INCLUDE=/opt/icu-69-plex/include
ENV PLEX_ICU_LIB=/opt/icu-69-plex/lib
ENV PLEX_ICU_LDFLAGS="-L${PLEX_ICU_LIB} -Wl,-rpath-link,${PLEX_ICU_LIB}"

ARG MIMALLOC_VERSION
ARG MIMALLOC_URL
ARG MIMALLOC_SHA512
RUN set -euo pipefail; \
    mimalloc_archive="/tmp/mimalloc-${MIMALLOC_VERSION}.tar.gz"; \
    mimalloc_src_dir="/tmp/mimalloc-${MIMALLOC_VERSION}"; \
    mimalloc_build_dir="${mimalloc_src_dir}/build"; \
    mimalloc_extra_cmake_flags=""; \
    if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      mimalloc_extra_cmake_flags="-DMI_LIBC_MUSL=ON"; \
    fi; \
    wget -O "${mimalloc_archive}" "${MIMALLOC_URL}"; \
    echo "${MIMALLOC_SHA512}  ${mimalloc_archive}" | sha512sum -c -; \
    tar -xzf "${mimalloc_archive}" -C /tmp; \
    cmake -S "${mimalloc_src_dir}" -B "${mimalloc_build_dir}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DMI_INSTALL_TOPLEVEL=ON \
      -DMI_OVERRIDE=ON \
      -DMI_BUILD_SHARED=OFF \
      -DMI_BUILD_STATIC=ON \
      -DMI_BUILD_OBJECT=ON \
      -DMI_BUILD_TESTS=OFF \
      -DMI_PADDING=OFF \
      -DMI_SECURE=0 \
      ${mimalloc_extra_cmake_flags}; \
    cmake --build "${mimalloc_build_dir}" --parallel "$(nproc)"; \
    cmake --install "${mimalloc_build_dir}" --prefix /opt/mimalloc; \
    printf "%s\n" "${MIMALLOC_SHA512}" > /opt/mimalloc/SHA512; \
    rm -rf "${mimalloc_archive}" "${mimalloc_src_dir}"

ENV MIMALLOC_VERSION="${MIMALLOC_VERSION}"
ENV MIMALLOC_URL="${MIMALLOC_URL}"
ENV MIMALLOC_SHA512="${MIMALLOC_SHA512}"
ENV MIMALLOC_OBJ=/opt/mimalloc/lib/mimalloc.o
ENV MIMALLOC_LIB=/opt/mimalloc/lib/libmimalloc.a

ARG MARCH
ARG SQLITE_AMALG_URL
ARG SQLITE_AMALG_SHA3_256
ARG SQLITE_SRC_URL=""
ARG SQLITE_SRC_SHA3_256=""
ARG RUN_BENCH=0

ENV SQLITE_AMALG_URL="${SQLITE_AMALG_URL}"
ENV SQLITE_AMALG_SHA3_256="${SQLITE_AMALG_SHA3_256}"
ENV SQLITE_SRC_URL="${SQLITE_SRC_URL}"
ENV SQLITE_SRC_SHA3_256="${SQLITE_SRC_SHA3_256}"
ENV RUN_BENCH="${RUN_BENCH}"

RUN LIBRARY_VARIANT="$LIBRARY_VARIANT" \
    MARCH="$MARCH" \
    SQLITE_AMALG_SHA3_256="$SQLITE_AMALG_SHA3_256" \
    SQLITE_SRC_URL="$SQLITE_SRC_URL" \
    SQLITE_SRC_SHA3_256="$SQLITE_SRC_SHA3_256" \
    MIMALLOC_VERSION="$MIMALLOC_VERSION" \
    MIMALLOC_URL="$MIMALLOC_URL" \
    MIMALLOC_SHA512="$MIMALLOC_SHA512" \
    MIMALLOC_OBJ="$MIMALLOC_OBJ" \
    MIMALLOC_LIB="$MIMALLOC_LIB" \
    /app/Build.sh library "$SQLITE_AMALG_URL"

# WHY: ldd and nm catch wrong ICU sonames or unsuffixed symbols before extraction.
RUN if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      set -euo pipefail; \
      LIB=$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so); \
      readelf -d "$LIB" || true; \
      nm -D "$LIB" | grep _plex | head -n 20 || true; \
      ldd_cmd="ldd"; \
      if ! command -v ldd >/dev/null 2>&1; then \
        ldd_cmd="$(ls /lib/ld-musl-*.so.1 | head -n 1) --list"; \
      fi; \
      for soname in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do \
        LD_LIBRARY_PATH="${PLEX_ICU_LIB}" ${ldd_cmd} "$LIB" | \
          awk -v s="$soname" '$1==s && $2=="=>" && $3!="not" { found=1 } \
                              END { exit !found }'; \
      done; \
      test "$(nm -D "$LIB" | grep -c _69_plex)" -gt 0; \
      ! nm -D "$LIB" | awk '$1=="U" && $2 ~ /_69$/ && $2 !~ /_69_plex$/ \
                             { found=1 } END { exit !found }'; \
    fi

RUN if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      set -euo pipefail; \
      LIB=$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so); \
      if nm -D --undefined-only "$LIB" | grep -E '(fcntl64|@GLIBC_)'; then \
        echo "FAIL: fcntl64 or GLIBC-versioned symbol found"; \
        exit 1; \
      fi; \
    fi

# WHY: Smoke links use LDFLAGS_VARIANT while runtime lookup uses LD_LIBRARY_PATH.
RUN set -euo pipefail; \
    if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)"); \
      AMALG_DIR=$(dirname "$LIB_DIR"); \
      LDFLAGS_VARIANT="${PLEX_ICU_LDFLAGS}"; \
      gcc -O0 -o /app/icu_smoke /app/tests/icu_smoke.c \
          -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT \
          -lsqlite3 -ldl -lpthread; \
      LD_LIBRARY_PATH="$LIB_DIR:${PLEX_ICU_LIB}" /app/icu_smoke; \
      cp /app/icu_smoke "$LIB_DIR/icu_smoke"; \
    fi

RUN set -euo pipefail; \
    LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)"); \
    AMALG_DIR=$(dirname "$LIB_DIR"); \
    LDFLAGS_VARIANT=""; LDPATH_VARIANT=""; \
    if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      LDFLAGS_VARIANT="${PLEX_ICU_LDFLAGS}"; LDPATH_VARIANT=":${PLEX_ICU_LIB}"; \
    fi; \
    gcc -O0 -o /app/config_after_dlopen_smoke /app/tests/config_after_dlopen_smoke.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/config_after_dlopen_smoke; \
    gcc -O0 -o /app/shutdown_reinit_smoke /app/tests/shutdown_reinit_smoke.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/shutdown_reinit_smoke; \
    gcc -O2 -o /app/config_after_dlopen_concurrent_bench \
        /app/tests/config_after_dlopen_concurrent_bench.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    gcc -O2 -o /app/alloc_latency_bench /app/tests/alloc_latency_bench.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    if [ "$RUN_BENCH" = "1" ]; then \
      LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/alloc_latency_bench; \
    fi

RUN set -euo pipefail; \
    LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)"); \
    AMALG_DIR=$(dirname "$LIB_DIR"); \
    LDFLAGS_VARIANT=""; LDPATH_VARIANT=""; \
    if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      LDFLAGS_VARIANT="${PLEX_ICU_LDFLAGS}"; LDPATH_VARIANT=":${PLEX_ICU_LIB}"; \
    fi; \
    gcc -O0 -o /app/auto_extension_smoke /app/tests/auto_extension_smoke.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/auto_extension_smoke

RUN set -euo pipefail; \
    LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)"); \
    AMALG_DIR=$(dirname "$LIB_DIR"); \
    LDFLAGS_VARIANT=""; LDPATH_VARIANT=""; \
    if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      LDFLAGS_VARIANT="${PLEX_ICU_LDFLAGS}"; LDPATH_VARIANT=":${PLEX_ICU_LIB}"; \
    fi; \
    gcc -O0 -DSLOW_QUERY_TRACKER_TEST_API -o /app/slow_query_smoke \
        /app/tests/slow_query_smoke.c /app/slow_query_tracker.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread -lm; \
    LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/slow_query_smoke; \
    gcc -O0 -DSLOW_QUERY_TRACKER_TEST_API -o /app/slow_query_atexit_smoke \
        /app/tests/slow_query_atexit_smoke.c /app/slow_query_tracker.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread -lm; \
    atexit_output="$(LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/slow_query_atexit_smoke 2>&1)"; \
    printf "%s\n" "$atexit_output"; \
    printf "%s\n" "$atexit_output" | grep -E 'slow_query_stats|dump_skipped'; \
    gcc -O0 -DSLOW_QUERY_TRACKER_TEST_API -o /app/slow_query_concurrency_smoke \
        /app/tests/slow_query_concurrency_smoke.c /app/slow_query_tracker.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread -lm; \
    LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/slow_query_concurrency_smoke; \
    gcc -O2 -o /app/slow_query_select1_bench /app/tests/slow_query_select1_bench.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    gcc -O2 -o /app/slow_query_metadata_bench /app/tests/slow_query_metadata_bench.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread
