#!/bin/bash

# ClusterTag Functional Evaluation Script
# This script builds and tests FFmpeg with ClusterTag sanitizer

set -e

echo "==================================="
echo "ClusterTag Functional Evaluation"
echo "==================================="
echo ""

# Navigate to evaluation directory
cd /workspace/evaluation/functional_evaluation

# Extract FFmpeg source
echo "1. Extracting FFmpeg source..."
if [ ! -d "ffmpeg" ]; then
    tar -xzf ffmpeg.tar.gz
    echo "   FFmpeg source extracted successfully"
else
    echo "   FFmpeg source already extracted"
fi

# Navigate to FFmpeg directory
cd FFmpeg-n4.4.6

# Configure FFmpeg with ClusterTag
echo ""
echo "2. Configuring FFmpeg with ClusterTag..."

# Use Debug build path for the evaluation
CLANG_PATH="/workspace/build/bin/clang"
CLANGXX_PATH="/workspace/build/bin/clang++"
FLAGS="-fuse-ld=lld-15 -fsanitize=clustertag"

./configure \
    --enable-debug \
    --disable-optimizations \
    --cc="$CLANG_PATH -shared-libsan  $FLAGS" \
    --cxx="$CLANGXX_PATH -shared-libsan $FLAGS" \
    --extra-ldflags="-shared-libsan $FLAGS"

echo "   FFmpeg configured successfully"

# Build FFmpeg
echo ""
echo "3. Building FFmpeg with ClusterTag..."
make -j$(nproc)
echo "   FFmpeg built successfully"

# Run functional test
echo ""
echo "4. Running functional test..."
echo "   Converting small.mp4 to small.gif..."

# Copy test file to ffmpeg directory
cp ../small.mp4 .

# Run the conversion test
./ffmpeg -y -i small.mp4 small.gif

if [ -f "small.gif" ]; then
    echo "   ✓ Functional test PASSED: small.gif created successfully"
    ls -la small.gif
else
    echo "   ✗ Functional test FAILED: small.gif not created"
    exit 1
fi

echo ""
echo "==================================="
echo "Functional Evaluation Completed"
echo "==================================="
echo ""
echo "Results:"
echo "- FFmpeg built successfully with ClusterTag"
echo "- Output file: small.gif"
