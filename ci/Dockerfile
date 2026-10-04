FROM python:3.12.3-slim-bookworm@sha256:fd3817f3a855f6c2ada16ac9468e5ee93e361005bd226fd5a5ee1a504e038c84

ARG ZEPHYR_SDK_VERSION=0.17.0
ARG SDK_BASE_URL=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0
ARG SDK_MINIMAL_SHA256=0514d2c684dfb5f6327374bfed0b3dcf727ff1500195d26b3730f98252fed095
ARG SDK_ARM_SHA256=c3992a788a0896ecc87f7fb3a3be3f234ec0509220bd4e9dbd99b22edd55e97f

LABEL org.opencontainers.image.source="https://github.com/3amon/leaderless-light-mesh"
LABEL org.opencontainers.image.description="Pinned ARM-only Zephyr build environment for thread-led-node"

ENV ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-0.17.0

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        device-tree-compiler \
        file \
        gcc \
        git \
        libc6-dev \
        make \
        ninja-build \
        wget \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

COPY requirements-build.txt /tmp/requirements-build.txt
RUN python3 -m pip install --no-cache-dir --requirement /tmp/requirements-build.txt \
    && rm /tmp/requirements-build.txt

RUN mkdir -p /opt/toolchains /tmp/zephyr-sdk \
    && wget --quiet --output-document=/tmp/zephyr-sdk/minimal.tar.xz \
        "${SDK_BASE_URL}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz" \
    && echo "${SDK_MINIMAL_SHA256}  /tmp/zephyr-sdk/minimal.tar.xz" | sha256sum --check \
    && tar -xJf /tmp/zephyr-sdk/minimal.tar.xz -C /opt/toolchains \
    && wget --quiet --output-document=/tmp/zephyr-sdk/arm.tar.xz \
        "${SDK_BASE_URL}/toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz" \
    && echo "${SDK_ARM_SHA256}  /tmp/zephyr-sdk/arm.tar.xz" | sha256sum --check \
    && tar -xJf /tmp/zephyr-sdk/arm.tar.xz -C "${ZEPHYR_SDK_INSTALL_DIR}" \
    && "${ZEPHYR_SDK_INSTALL_DIR}/setup.sh" -h \
    && rm -rf /tmp/zephyr-sdk \
    && python3 --version \
    && cmake --version \
    && west --version \
    && "${ZEPHYR_SDK_INSTALL_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" --version
