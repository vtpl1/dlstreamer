FROM intel/dlstreamer:2024.1.0-dev-ubuntu22

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

RUN wget -O cmake.sh https://github.com/Kitware/CMake/releases/download/v3.28.5/cmake-3.28.5-linux-x86_64.sh && \
    sh cmake.sh --prefix=/usr/local/ --exclude-subdir && rm -rf cmake.sh

# RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -
# ARG LLVM_VERSION=18
# RUN wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh ${LLVM_VERSION} all
# ENV PATH="${PATH}:/usr/lib/llvm-${LLVM_VERSION}/bin"

USER dlstreamer
