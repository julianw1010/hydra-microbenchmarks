#!/bin/bash

# microbenchmark4_runner.sh - Memory Operation Comparison Benchmark Runner
#
# Compares Hydra's effectiveness across mprotect, munmap, and mmap operations
# Based on Hydra paper Figure 9
#
# Compile benchmark first:
#   gcc -O2 -o microbenchmark4 microbenchmark4.c -lpthread -lnuma
#
# Run as root: sudo ./microbenchmark4_runner.sh

set -e

BENCH="./microbenchmark4"
HYDRA_HISTORY="/proc/hydra/history"
SPINNERS=8  # Spinners per remote node

# Operations to test
OPERATIONS=(mprotect munmap mmap_full)

# Check prerequisites
if [ ! -x "$BENCH" ]; then
    echo "Error: $BENCH not found or not executable"
    echo "Compile with: gcc -O2 -o microbenchmark4 microbenchmark4.c -lpthread -lnuma"
    exit 1
fi

if [ ! -f "$HYDRA_HISTORY" ]; then
    echo "Error: $HYDRA_HISTORY not found - Hydra kernel required"
    exit 1
fi

echo "========================================================"
echo "Microbenchmark 4: Memory Operation Comparison"
echo "========================================================"
echo "Operations: ${OPERATIONS[*]}"
echo "Spinners per remote node: $SPINNERS"
echo "Each test runs WITHOUT Hydra, then WITH Hydra"
echo "========================================================"
echo ""

for op in "${OPERATIONS[@]}"; do
    echo ""
    echo "########################################################"
    echo "# Testing operation: $op"
    echo "########################################################"
    
    # --- WITHOUT HYDRA ---
    echo ""
    echo ">>> WITHOUT HYDRA (baseline Linux):"
    echo ""
    
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    echo -1 > "$HYDRA_HISTORY"
    sleep 0.5
    
    $BENCH -o $op -s $SPINNERS
    
    echo ""
    echo "Hydra IPI Statistics (without Hydra):"
    echo "----------------------------------------"
    cat "$HYDRA_HISTORY"
    echo "----------------------------------------"
    
    sleep 1
    
    # --- WITH HYDRA ---
    echo ""
    echo ">>> WITH HYDRA (numactl -r all):"
    echo ""
    
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    echo -1 > "$HYDRA_HISTORY"
    sleep 0.5
    
    numactl -r all $BENCH -o $op -s $SPINNERS
    
    echo ""
    echo "Hydra IPI Statistics (with Hydra):"
    echo "----------------------------------------"
    cat "$HYDRA_HISTORY"
    echo "----------------------------------------"
    
    sleep 1
done

echo ""
echo "========================================================"
echo "All tests completed"
echo "========================================================"
