FROM intel/dlstreamer:2024.1.2-dev-ubuntu22

USER root

RUN apt update \
    && DEBIAN_FRONTEND=noninteractive apt install -y \
    ca-certificates \
    software-properties-common \
    build-essential \
    wget curl git gdb ninja-build \
    lsb-release \
    pkg-config zip unzip tar iputils-ping ccache nasm \
    bison cpio autoconf-archive autotools-dev automake libtool gperf \
    gpg-agent flex graphviz cmake \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

USER dlstreamer
