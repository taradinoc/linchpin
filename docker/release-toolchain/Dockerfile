FROM mcr.microsoft.com/dotnet/sdk:10.0-noble

ARG DEBIAN_FRONTEND=noninteractive
ARG LLVM_MOS_VERSION=v23.0.0
ARG AMIGA_GCC_REF=master

RUN apt-get update \
    && apt-get install -y software-properties-common \
    && add-apt-repository -y ppa:vriviere/ppa \
    && apt-get update \
    && apt-get install -y \
        autoconf \
        automake \
        bison \
        ca-certificates \
        cross-mint-essential \
        curl \
        flex \
        gcc \
        g++ \
        gettext \
        git \
        gperf \
        libgmp-dev \
        libmpc-dev \
        libmpfr-dev \
        libncurses-dev \
        libreadline-dev \
        lhasa \
        make \
        patch \
        perl \
        python3 \
        rsync \
        texinfo \
        wget \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/llvm-mos \
    && curl -L --fail --retry 5 \
        "https://github.com/llvm-mos/llvm-mos-sdk/releases/download/${LLVM_MOS_VERSION}/llvm-mos-linux.tar.xz" \
        -o /tmp/llvm-mos.tar.xz \
    && tar -xJf /tmp/llvm-mos.tar.xz -C /opt/llvm-mos --strip-components=1 \
    && rm -f /tmp/llvm-mos.tar.xz

RUN mkdir -p /opt/amiga \
    && git clone https://github.com/AmigaPorts/m68k-amigaos-gcc.git /tmp/m68k-amigaos-gcc \
    && cd /tmp/m68k-amigaos-gcc \
    && git checkout "${AMIGA_GCC_REF}" \
    && make update \
    && make -j"$(nproc)" PREFIX=/opt/amiga all \
    && rm -rf /tmp/m68k-amigaos-gcc

ENV PATH="/opt/amiga/bin:/opt/llvm-mos/bin:${PATH}"
