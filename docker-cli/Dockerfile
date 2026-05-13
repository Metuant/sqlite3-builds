# syntax=docker/dockerfile:1
FROM alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc

LABEL org.sqlite.static.binary.linux.x86_64.dockerfile.version="2.0.0"

ARG URL_SQLITE_SOURCE_ZIP
ARG COMPRESS_SQLITE3
ARG MARCH
ARG SQLITE_SHA3_256

# Version of SQLite to download (full url to sqlite-amalgamation.zip)
ENV SQLITE_URL_SRC="${URL_SQLITE_SOURCE_ZIP}"
ENV COMPRESSOR="${COMPRESS_SQLITE3}"
ENV MARCH="${MARCH}"
ENV SQLITE_SHA3_256="${SQLITE_SHA3_256}"
ENV CC="ccache gcc"
ENV CCACHE_DIR="/ccache"

RUN echo "**** Install Dependencies ****" && \
    echo >> /etc/apk/repositories && \
    echo "http://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories &&  \
    apk update && apk upgrade &&  \
    apk add --no-cache \
        fakeroot \
        musl-dev \
        linux-headers \
        gcc \
        libgcc \
        ccache \
        coreutils \
        binutils \
        upx \
        file \
        wget \
        unzip \
        zlib-dev \
        zlib-static \
        libhistory \
        readline-static \
        readline-dev \
        ncurses-static \
        ncurses-dev

WORKDIR /app

COPY Build.sh /app/
RUN --mount=type=cache,target=/ccache chmod +x /app/Build.sh && ./Build.sh cli "${SQLITE_URL_SRC}" "${COMPRESSOR}"
