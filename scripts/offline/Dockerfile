FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    liblua5.4-dev \
    lua5.4 \
    clang-format \
    git \
    ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
