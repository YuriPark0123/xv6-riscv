FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    gdb \
    qemu-system-misc \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    make \
    && rm -rf /var/lib/apt/lists/*

# WORKDIR /xv6s
WORKDIR /workspace
# RUN git clone https://github.com/mit-pdos/xv6-riscv.git .

CMD ["/bin/bash"]