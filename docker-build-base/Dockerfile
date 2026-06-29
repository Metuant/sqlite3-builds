# syntax=docker/dockerfile:1.5
ARG BASEIMAGE_UBUNTU
FROM ${BASEIMAGE_UBUNTU}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG BASEIMAGE_UBUNTU
ARG CMAKE_VERSION
ARG CMAKE_SHA256_X86_64
ARG CMAKE_SHA256_AARCH64
ARG UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT

LABEL org.sqlite3-builds.base.ubuntu="${BASEIMAGE_UBUNTU}" \
      org.sqlite3-builds.cmake.version="${CMAKE_VERSION}" \
      org.sqlite3-builds.ubuntu-toolchain-r-test.url="https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu" \
      org.sqlite3-builds.ubuntu-toolchain-r-test.codename="bionic" \
      org.sqlite3-builds.ubuntu-toolchain-r-test.key-fingerprint="${UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT}"

COPY ubuntu-toolchain-r-test.asc /tmp/ubuntu-toolchain-r-test.asc

RUN printf '%s\n' \
      'Acquire::http::Timeout "30";' \
      'Acquire::https::Timeout "30";' \
      'Acquire::Retries "3";' \
      > /etc/apt/apt.conf.d/99sqlite-build-timeouts

RUN set -euo pipefail; \
    retry() { \
      local attempt=1 max_attempts=3 backoff=5; \
      while true; do \
        if "$@"; then \
          return 0; \
        fi; \
        if [ "${attempt}" -ge "${max_attempts}" ]; then \
          echo "FATAL: command failed after ${attempt} attempts: $*" >&2; \
          return 1; \
        fi; \
        echo "WARN: command failed (attempt ${attempt}/${max_attempts}), retrying in ${backoff}s: $*" >&2; \
        sleep "${backoff}"; \
        attempt=$((attempt + 1)); \
        backoff=$((backoff * 2)); \
      done; \
    }; \
    retry apt-get -y update; \
    retry apt-get -y install --no-install-recommends \
      ca-certificates \
      gnupg \
      wget \
      openssl; \
    keyring_dir="$(mktemp -d)"; \
    chmod 700 "${keyring_dir}"; \
    gpg --batch --homedir "${keyring_dir}" --import /tmp/ubuntu-toolchain-r-test.asc; \
    gpg --batch --homedir "${keyring_dir}" --with-colons --list-keys --fingerprint \
      | awk -F: '$1=="pub"{primary=1; next} primary && $1=="fpr"{print $10; primary=0; next} $1=="sub"{primary=0}' > /tmp/ubuntu-toolchain-r-test.fpr; \
    rm -rf "${keyring_dir}"; \
    fingerprint_count="$(wc -l < /tmp/ubuntu-toolchain-r-test.fpr | tr -d ' ')"; \
    if [ "${fingerprint_count}" != "1" ]; then \
      echo "FATAL: expected exactly one toolchain key fingerprint, found ${fingerprint_count}" >&2; \
      exit 1; \
    fi; \
    fingerprint="$(sed -n '1p' /tmp/ubuntu-toolchain-r-test.fpr)"; \
    if [ "${fingerprint}" != "${UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT}" ]; then \
      echo "FATAL: unexpected toolchain key fingerprint: ${fingerprint}" >&2; \
      exit 1; \
    fi; \
    gpg --batch --dearmor -o /usr/share/keyrings/ubuntu-toolchain-r-test.gpg /tmp/ubuntu-toolchain-r-test.asc; \
    printf 'deb [arch=%s signed-by=/usr/share/keyrings/ubuntu-toolchain-r-test.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu bionic main\n' "$(dpkg --print-architecture)" \
      > /etc/apt/sources.list.d/ubuntu-toolchain-r-test.list; \
    retry apt-get -y update; \
    retry apt-get -y install --no-install-recommends \
      gcc-13 \
      g++-13 \
      make \
      binutils \
      patch \
      file \
      unzip \
      zlib1g \
      zlib1g-dev; \
    rm -rf /var/lib/apt/lists/* /tmp/ubuntu-toolchain-r-test.asc /tmp/ubuntu-toolchain-r-test.fpr

RUN set -euo pipefail; \
    arch="$(uname -m)"; \
    case "${arch}" in \
      x86_64) cmake_sha256="${CMAKE_SHA256_X86_64}" ;; \
      aarch64) cmake_sha256="${CMAKE_SHA256_AARCH64}" ;; \
      *) echo "FATAL: unsupported CMake architecture: ${arch}" >&2; exit 1 ;; \
    esac; \
    cmake_archive="/tmp/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    wget --tries=3 --timeout=30 --waitretry=5 --retry-connrefused \
      -O "${cmake_archive}" \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    echo "${cmake_sha256}  ${cmake_archive}" | sha256sum -c -; \
    tar -xzf "${cmake_archive}" --strip-components=1 -C /usr/local; \
    rm -f "${cmake_archive}"; \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100; \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100; \
    update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-13 100; \
    update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100

RUN set -euo pipefail; \
    for compiler in gcc g++ cc c++; do \
      major="$("${compiler}" -dumpversion | awk -F. '{print $1}')"; \
      if [ "${major}" != "13" ]; then \
        echo "FATAL: ${compiler} major version is ${major}, expected 13" >&2; \
        exit 1; \
      fi; \
    done; \
    if [ "$(cmake --version | sed -n '1p')" != "cmake version ${CMAKE_VERSION}" ]; then \
      cmake --version >&2; \
      exit 1; \
    fi; \
    if [ "$(ldd --version | awk 'NR==1{print $NF}')" != "2.27" ]; then \
      ldd --version >&2; \
      exit 1; \
    fi; \
    grep -F 'https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu bionic main' /etc/apt/sources.list.d/ubuntu-toolchain-r-test.list; \
    sha3="$(printf 'sqlite3-builds\n' | openssl dgst -sha3-256 | awk '{print $NF}')"; \
    if [ "${sha3}" != "345bd227155241c08ab64f6a3e34ec2b1b590f4b9c98087dfef0d7e70dbe578b" ]; then \
      echo "FATAL: OpenSSL SHA3 smoke mismatch: ${sha3}" >&2; \
      exit 1; \
    fi
