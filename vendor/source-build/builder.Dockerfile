# remora-builder — the containerized AOSP/Lineage build environment that do-build.sh
# runs inside (tree mounted at /src). Rebuilt automatically by 'Build from source' when the
# image is missing: docker build -t remora-builder:latest -f builder.Dockerfile .
#
# The canonical AOSP ubuntu build-dependency set (source.android.com) + the extras a
# LineageOS 23 / Android 16 tree needs (rsync, bc, lz4, ccache). Java/clang/ninja all come
# prebuilt inside the tree itself.
FROM ubuntu:22.04

# bump to force 'Build from source' to rebuild the image (it checks this label)
LABEL remora.builder="2"

ENV DEBIAN_FRONTEND=noninteractive
# openssl: the CLI, not just libssl — avbtool execs it for APEX/AVB signing and the
# docker ubuntu base doesn't ship it (fails at ~74% with "No such file: 'openssl'")
RUN apt-get update && apt-get install -y --no-install-recommends \
    bc bison build-essential ccache curl flex fontconfig git-core gnupg \
    libc6-dev-i386 libgl1-mesa-dev liblz4-tool libncurses5-dev libssl-dev \
    libx11-dev libxml2-utils lib32z1-dev openssl python3 python-is-python3 \
    rsync unzip x11proto-core-dev xsltproc zip zlib1g-dev file \
    && rm -rf /var/lib/apt/lists/*

# The tree is bind-mounted and host-owned; the build runs as root in here — silence git's
# dubious-ownership refusals (soong shells out to git for build numbers).
RUN git config --global --add safe.directory '*' \
    && git config --global user.name Remora \
    && git config --global user.email remora@localhost

WORKDIR /src
