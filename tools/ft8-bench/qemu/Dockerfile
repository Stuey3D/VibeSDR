# The FT8 instruction-count image (2026-10-25): qemu-user with TCG plugins, gcc for the plugin, the ARM binutils
# for the symbol map, and the x86 cross compilers. Build: docker build -t ft8perf-x86 tools/ft8-bench/qemu
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends qemu-user gcc libc6-dev libglib2.0-dev \
      binutils-arm-linux-gnueabihf g++-x86-64-linux-gnu g++-i686-linux-gnu libc6-dev-amd64-cross libc6-dev-i386-cross \
      python3 curl ca-certificates && rm -rf /var/lib/apt/lists/*
