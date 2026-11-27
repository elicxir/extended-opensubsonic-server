# Build and test environment for eos (Ubuntu 26.04).
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build pkg-config \
        libsqlite3-dev libtag-dev libssl-dev libutf8proc-dev \
        nlohmann-json3-dev libtomlplusplus-dev libgtest-dev \
        libdrogon-dev libjsoncpp-dev libstb-dev \
        flac curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
