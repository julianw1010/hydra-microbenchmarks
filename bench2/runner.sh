#!/bin/bash

# microbenchmark2_runner.sh - Region Size Scaling Benchmark Runner
#
# Compile benchmark first:
#   gcc -O2 -o microbenchmark2 microbenchmark2.c -lpthread -lnuma
#
# Run as root: sudo ./microbenchmark2_runner.sh

set -e

BENCH="./microbenchmark2"
HYDRA_HISTORY="/proc/hydra/history"

# Region sizes in KB: 4KB to 128MB
SIZES=(4 64 512 2048 8192 32768 131072)

# Check prerequisites
if [ ! -x "$BENCH" ]; then
    echo "Error: $BENCH not found or not executable"
    echo "Compile with: gcc -O2 -o microbenchmark2 microbenchmark2.c -lpthread -lnuma"
    exit 1
fi

if [ ! -f "$HYDRA_HISTORY" ]; then
    echo "Error: $HYDRA_HISTORY not found - Hydra kernel required"
    exit 1
fi

echo "========================================================"
echo "Microbenchmark 2: Region Size Scaling"
echo "========================================================"
echo "Region sizes (KB): ${SIZES[*]}"
echo "========================================================"
echo ""

for size in "${SIZES[@]}"; do
    echo ""
    echo "########################################################"
    echo "# Testing region size: ${size}KB"
    echo "########################################################"
    echo ""
    
    # Drop caches
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    
    # Reset Hydra stats
    echo -1 > "$HYDRA_HISTORY"
    
    sleep 0.5
    
    # Run benchmark with Hydra enabled
    numactl -r all $BENCH -s $size
    
    # Print Hydra IPI statistics
    echo ""
    echo "Hydra IPI Statistics:"
    echo "----------------------------------------"
    cat "$HYDRA_HISTORY"
    echo "----------------------------------------"
    
    sleep 1
done

echo ""
echo "========================================================"
echo "All tests completed"
echo "========================================================"
