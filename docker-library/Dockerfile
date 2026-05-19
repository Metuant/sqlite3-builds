# syntax=docker/dockerfile:1.5
ARG LIBRARY_VARIANT=generic

FROM ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b AS base-generic

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
        gcc \
        binutils \
        patch \
        zlib1g \
        zlib1g-dev

FROM alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc AS base-plex

RUN echo "**** Install Dependencies ****" && \
    apk add --no-cache \
        autoconf \
        bash \
        binutils \
        ca-certificates \
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

ARG LIBRARY_VARIANT=generic

WORKDIR /app

COPY build/Build.sh /app/Build.sh
COPY src/auto_extension.c /app/auto_extension.c
COPY src/observability.c /app/observability.c
COPY tools/sqlite-amalgamation.patch /app/tools/sqlite-amalgamation.patch
COPY tools/expected-sqlite-config-count.txt /app/tools/expected-sqlite-config-count.txt
COPY tools/expected-sqlite-dbconfig-count.txt /app/tools/expected-sqlite-dbconfig-count.txt
COPY tests/ /app/tests/

RUN chmod +x /app/Build.sh

ARG ICU_SHA512=d4aeb781715144ea6e3c6b98df5bbe0490bfa3175221a1d667f3e6851b7bd4a638fa4a37d4a921ccb31f02b5d15a6dded9464d98051964a86f7b1cde0ff0aab7
RUN if [ "$LIBRARY_VARIANT" = "plex" ]; then \
      set -euo pipefail; \
      wget -O /tmp/icu4c-69_1-src.tgz https://github.com/unicode-org/icu/releases/download/release-69-1/icu4c-69_1-src.tgz; \
      echo "${ICU_SHA512}  /tmp/icu4c-69_1-src.tgz" | sha512sum -c -; \
      tar -xzf /tmp/icu4c-69_1-src.tgz -C /tmp; \
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

ARG MARCH
ARG SQLITE_AMALG_URL
ARG SQLITE_AMALG_SHA3_256
ARG SQLITE_SRC_URL=""
ARG SQLITE_SRC_SHA3_256=""

ENV SQLITE_AMALG_URL="${SQLITE_AMALG_URL}"
ENV SQLITE_AMALG_SHA3_256="${SQLITE_AMALG_SHA3_256}"
ENV SQLITE_SRC_URL="${SQLITE_SRC_URL}"
ENV SQLITE_SRC_SHA3_256="${SQLITE_SRC_SHA3_256}"

RUN LIBRARY_VARIANT="$LIBRARY_VARIANT" \
    MARCH="$MARCH" \
    SQLITE_AMALG_SHA3_256="$SQLITE_AMALG_SHA3_256" \
    SQLITE_SRC_URL="$SQLITE_SRC_URL" \
    SQLITE_SRC_SHA3_256="$SQLITE_SRC_SHA3_256" \
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
    gcc -O0 -o /app/auto_extension_smoke /app/tests/auto_extension_smoke.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" $LDFLAGS_VARIANT -lsqlite3 -ldl -lpthread; \
    LD_LIBRARY_PATH="$LIB_DIR${LDPATH_VARIANT}" /app/auto_extension_smoke
