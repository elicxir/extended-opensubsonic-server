# Runtime image for eos. Build: docker build -t eos .   Run: see compose.yaml.

FROM ubuntu:26.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build pkg-config \
        libsqlite3-dev libtag-dev libssl-dev libutf8proc-dev \
        nlohmann-json3-dev libtomlplusplus-dev libgtest-dev \
        libdrogon-dev libjsoncpp-dev libstb-dev flac \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY server server
COPY tools tools
COPY tests tests
ARG RUN_TESTS=1
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build \
    && if [ "$RUN_TESTS" = "1" ]; then ctest --test-dir build --output-on-failure; fi \
    && cmake --install build --prefix /out --strip

FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libtag2 libutf8proc3 libssl3t64 libtomlplusplus3t64 libdrogon1t64 ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data /music /etc/eos \
    && chown 1000:1000 /data
COPY --from=build /out/bin/ /usr/local/bin/
COPY docker/eos.toml /etc/eos/eos.toml
ENV EOS_CONFIG=/etc/eos/eos.toml
VOLUME ["/data"]
EXPOSE 4540
USER 1000:1000
ENTRYPOINT ["eosd"]
