# A Linux build environment for Project Predation, for use from a Windows machine that has Docker.
#
# The port had never been compiled: everything in Docs/LINUX.md was reasoned rather than proved.
# This is the smallest thing that turns that into a real compiler's opinion.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Two groups. The first is the toolchain and the things vcpkg itself needs to fetch and build a
# port. The second is the development packages the ports look for on the system: SDL3 wants X11,
# Wayland, ALSA and udev headers, and bgfx wants OpenGL and X11.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ ninja-build cmake git curl zip unzip tar pkg-config \
        autoconf automake libtool m4 python3 python3-jinja2 ca-certificates bison flex \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev \
        libxss-dev libxkbcommon-dev libxfixes-dev libxrender-dev \
        libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libglu1-mesa-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
    && rm -rf /var/lib/apt/lists/*

# vcpkg lives in the image so that starting a container does not mean cloning it again. The
# dependencies it builds do not: those go in a volume, because they are gigabytes and they should
# survive the image being rebuilt.
#
# The clone is deliberately not shallow. vcpkg.json pins a builtin-baseline, and vcpkg reads that
# commit out of the repository's own history; a --depth 1 clone does not have it and the configure
# fails on a git error that says nothing about baselines.
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics
ENV PATH="${VCPKG_ROOT}:${PATH}"

WORKDIR /src
