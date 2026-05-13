FROM ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b

LABEL org.sqlite.static.library.linux.x86_64.dockerfile.version="2.0.0"

ARG URL_SQLITE_SOURCE_ZIP
ARG MARCH
ARG SQLITE_SHA3_256

# Version of SQLite to download (full url to sqlite-amalgamation.zip)
ENV SQLITE_URL_SRC="${URL_SQLITE_SOURCE_ZIP}"
ENV MARCH="${MARCH}"
ENV SQLITE_SHA3_256="${SQLITE_SHA3_256}"

RUN echo "**** Install Dependencies ****" && \
    apt-get -y update && \
    apt-get -y upgrade && \
    apt-get -y install --no-install-recommends \
        apt-transport-https \
        ca-certificates \
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
        zlib1g \
        zlib1g-dev

WORKDIR /app

COPY Build.sh /app/
RUN  chmod +x /app/Build.sh && ./Build.sh library "${SQLITE_URL_SRC}"
