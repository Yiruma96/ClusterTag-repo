#!/bin/bash

# ClusterTag FFmpeg Evaluation Script
# This script demonstrates FFmpeg compilation and execution with ClusterTag

set -e

echo "==================================="
echo "ClusterTag FFmpeg Evaluation"
echo "==================================="
echo ""

# Check if we're in the container
if [ ! -f "/workspace/build/bin/clang" ]; then
    echo "Error: ClusterTag build not found. Please run this script inside the container."
    echo "Build ClusterTag first with: ./build.sh"
    echo "Then run: docker run -it clustertag"
    exit 1
fi

# Navigate to evaluation directory
cd /workspace/evaluation

echo "1. Extracting FFmpeg source..."
if [ ! -d "ffmpeg" ]; then
    tar -xzf ffmpeg.tar.gz
    echo "   ✓ FFmpeg source extracted"
else
    echo "   ✓ FFmpeg source already available"
fi

# Navigate to FFmpeg directory
cd ffmpeg

echo ""
echo "2. Configuring FFmpeg with ClusterTag..."

# Use the correct paths for ClusterTag build
CLANG_PATH="/workspace/build/bin/clang"
CLANGXX_PATH="/workspace/build/bin/clang++"

./configure \
    --enable-debug \
    --disable-optimizations \
    --cc="$CLANG_PATH -shared-libsan -fsanitize=clustertag" \
    --cxx="$CLANGXX_PATH -shared-libsan -fsanitize=clustertag" \
    --extra-ldflags="-shared-libsan -fsanitize=clustertag" >/dev/null 2>&1

echo "   ✓ FFmpeg configured with ClusterTag sanitizer"

echo ""
echo "3. Building FFmpeg (this may take several minutes)..."
make -j$(nproc) >/dev/null 2>&1
echo "   ✓ FFmpeg built successfully with ClusterTag"

echo ""
echo "4. Running FFmpeg test..."
cp ../small.mp4 .

echo "   Converting small.mp4 to small.gif using ClusterTag-enhanced FFmpeg..."
./ffmpeg -y -i small.mp4 small.gif >/dev/null 2>&1

if [ -f "small.gif" ]; then
    echo "   ✓ Video conversion successful!"
    echo ""
    echo "==================================="
    echo "Evaluation Results"
    echo "==================================="
    echo "✓ FFmpeg compilation with ClusterTag: SUCCESS"
    echo "✓ Video processing functionality: SUCCESS"
    echo "✓ ClusterTag integration: WORKING"
    echo ""
    echo "Output file details:"
    ls -lh small.gif
else
    echo "   ✗ Video conversion failed"
    exit 1
fi

echo ""
echo "ClusterTag evaluation completed successfully!"
echo "FFmpeg is working correctly with ClusterTag sanitizer."
