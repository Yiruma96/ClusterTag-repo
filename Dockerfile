# ClusterTag LLVM Build Dockerfile
# Based on LLVM 15.0.0 with ClusterTag modifications

FROM registry.cn-hangzhou.aliyuncs.com/acs/ubuntu:22.04

# Set non-interactive mode for apt
ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    python3 \
    python3-pip \
    llvm-15 \
    llvm-15-dev \
    clang-15 \
    lld-15 \
    libc++-15-dev \
    libc++abi-15-dev \
    libxml2-dev \
    libz-dev \
    zlib1g-dev \
    pkg-config \
    vim \
    yasm \
    nasm \
    wget \
    curl \
    bc \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Clone LLVM project and checkout the specific tag
RUN git clone https://gitee.com/mirrors/llvm-project.git llvm-project \
    && cd llvm-project \
    && git checkout llvmorg-15.0.0

# Copy ClusterTag source files
COPY src/ /workspace/llvm-project/

# Copy evaluation materials
COPY evaluation/ /workspace/evaluation/

# Set executable permissions for test scripts
RUN chmod +x /workspace/evaluation/security_evaluation/security_test.sh \
    && chmod +x /workspace/evaluation/functional_evaluation/functional_test.sh

# Create build directory
RUN mkdir -p /workspace/build

# Set working directory to build
WORKDIR /workspace/build

# Configure and build LLVM with ClusterTag modifications
RUN cmake -G Ninja \
    -DLLVM_ENABLE_RTTI=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_BUILD_TOOLS=OFF \
    -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" \
    /workspace/llvm-project/llvm

# Build the project
RUN ninja

# Set the final working directory
WORKDIR /workspace

# Default command
CMD ["/bin/bash"]
