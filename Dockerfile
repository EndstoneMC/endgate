# syntax=docker/dockerfile:1

ARG UBUNTU_VERSION=24.04

FROM ubuntu:${UBUNTU_VERSION} AS builder

ARG LLVM_VERSION=22

ENV DEBIAN_FRONTEND=noninteractive \
    UV_TOOL_DIR=/opt/uv/tools \
    UV_TOOL_BIN_DIR=/usr/local/bin \
    CC=clang-${LLVM_VERSION} \
    CXX=clang++-${LLVM_VERSION}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update -y -qq \
    && apt-get install -y -qq --no-install-recommends build-essential ca-certificates curl git perl \
    && curl -fsSL --retry 10 --retry-delay 5 --retry-all-errors https://apt.llvm.org/llvm-snapshot.gpg.key \
        -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && CODENAME=$(. /etc/os-release && echo "${VERSION_CODENAME}") \
    && echo "deb https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update -y -qq \
    && apt-get install -y -qq --no-install-recommends clang-${LLVM_VERSION} \
    && rm -rf /var/lib/apt/lists/*

COPY --from=ghcr.io/astral-sh/uv:0.9 /uv /uvx /usr/local/bin/

RUN uv tool install conan \
    && uv tool install cmake \
    && uv tool install ninja \
    && conan profile detect --force

WORKDIR /usr/src/endgate

COPY conanfile.txt conanfile.txt
RUN conan install . \
        -s '&:build_type=RelWithDebInfo' \
        -s compiler.cppstd=23 \
        -c tools.cmake.cmaketoolchain:generator=Ninja \
        -c "tools.build:compiler_executables={\"c\":\"clang-${LLVM_VERSION}\",\"cpp\":\"clang++-${LLVM_VERSION}\"}" \
        --build=missing

COPY . .

RUN cmake --preset conan-relwithdebinfo \
    && cmake --build --preset conan-relwithdebinfo --target endgate \
    && strip --strip-debug -o /usr/local/bin/endgate build/RelWithDebInfo/endgate

FROM ubuntu:${UBUNTU_VERSION} AS final

LABEL org.opencontainers.image.title="Endgate" \
      org.opencontainers.image.description="A proxy for Minecraft: Bedrock Edition built on Velocity's design." \
      org.opencontainers.image.source="https://github.com/EndstoneMC/endgate" \
      org.opencontainers.image.licenses="AGPL-3.0-only" \
      org.opencontainers.image.vendor="EndstoneMC"

ENV TZ=UTC

RUN apt-get update -y -qq \
    && apt-get install -y -qq --no-install-recommends ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && (userdel --remove ubuntu || true) \
    && groupadd --gid 1000 endgate \
    && useradd --uid 1000 --gid 1000 --create-home --shell /usr/sbin/nologin endgate \
    && mkdir -p /data \
    && chown endgate:endgate /data

COPY --from=builder /usr/local/bin/endgate /usr/local/bin/endgate

USER endgate
WORKDIR /data
VOLUME ["/data"]

EXPOSE 19132/udp 19132/tcp

STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/local/bin/endgate"]
