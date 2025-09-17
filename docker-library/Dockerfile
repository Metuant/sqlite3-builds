FROM ubuntu:24.04

LABEL org.sqlite.static.library.linux.x86_64.dockerfile.version="2.0.0"

ARG URL_SQLITE_SOURCE_ZIP

# Version of SQLite to download (full url to sqlite-amalgamation.zip)
ENV SQLITE_URL_SRC="${URL_SQLITE_SOURCE_ZIP}"

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
    apt -y install \
        gcc \
        binutils \
        zlib1g \
        zlib1g-dev

WORKDIR /app

COPY Build.sh /app/
RUN  chmod +x /app/Build.sh && ./Build.sh "${SQLITE_URL_SRC}"
