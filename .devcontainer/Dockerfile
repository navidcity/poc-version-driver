# SPDX-License-Identifier: GPL-2.0-only
#
# Build environment for the poc-version driver: packages only. The kernel
# trees are not in the image: scripts/setup-kernels.sh clones them into the
# poc-version-kernels named volume mounted at /opt/kernels, so rebuilding
# the image doesn't download them again.
#
# Debian bookworm ships GCC 12, which builds v5.15, v6.4 and current kernels.
FROM debian:bookworm

ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        # host toolchain and kernel build dependencies
        bc bison build-essential ca-certificates cpio flex kmod \
        libelf-dev libncurses-dev libssl-dev rsync xz-utils zstd \
        # cross-compilers for the boards
        gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf \
        # checkpatch, kunit.py and the rest of the tooling
        bear clang-format device-tree-compiler git perl python3 \
        less procps sudo \
        # make dt-binding-check
        dt-schema yamllint \
    && rm -rf /var/lib/apt/lists/*

# Non-root user, so files created in the bind-mounted workspace belong to the
# developer. /opt/kernels is created and owned here: an empty named volume
# copies this ownership when it is first mounted.
RUN groupadd --gid "$USER_GID" "$USERNAME" \
    && useradd --uid "$USER_UID" --gid "$USER_GID" -m -s /bin/bash "$USERNAME" \
    && echo "$USERNAME ALL=(root) NOPASSWD:ALL" > "/etc/sudoers.d/$USERNAME" \
    && chmod 0440 "/etc/sudoers.d/$USERNAME" \
    && mkdir -p /opt/kernels \
    && chown "$USERNAME:$USERNAME" /opt/kernels

ENV KERNELS=/opt/kernels
USER $USERNAME
