#!/bin/bash

# microbenchmark3_runner.sh - Spinning Thread Interference Benchmark Runner
#
# Reproduces Hydra paper Figure 1: mprotect performance vs spinning threads
# Runs each configuration WITHOUT and WITH Hydra for comparison
#
# Compile benchmark first:
#   gcc -O2 -o microbenchmark3 microbenchmark3.c -lpthread -lnuma
#
# Run as root: sudo ./microbenchmark3_runner.sh

set -e

BENCH="./microbenchmark3"
HYDRA_HISTORY="/proc/hydra/history"

# Spinner counts per remote node
SPINNER_COUNTS=(0 1 2 4 8 16)

# Check prerequisites
if [ ! -x "$BENCH" ]; then
    echo "Error: $BENCH not found or not executable"
    echo "Compile with: gcc -O2 -o microbenchmark3 microbenchmark3.c -lpthread -lnuma"
    exit 1
fi

if [ ! -f "$HYDRA_HISTORY" ]; then
    echo "Error: $HYDRA_HISTORY not found - Hydra kernel required"
    exit 1
fi

echo "========================================================"
echo "Microbenchmark 3: Spinning Thread Interference"
echo "========================================================"
echo "Spinner counts per node: ${SPINNER_COUNTS[*]}"
echo "Each test runs WITHOUT Hydra, then WITH Hydra"
echo "========================================================"
echo ""

for spinners in "${SPINNER_COUNTS[@]}"; do
    echo ""
    echo "########################################################"
    echo "# Testing with $spinners spinners per remote node"
    echo "########################################################"
    
    # --- WITHOUT HYDRA ---
    echo ""
    echo ">>> WITHOUT HYDRA (baseline Linux):"
    echo ""
    
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    echo -1 > "$HYDRA_HISTORY"
    sleep 0.5
    
    # Run WITHOUT numactl -r all
    $BENCH -s $spinners
    
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
    
    # Run WITH numactl -r all
    numactl -r all $BENCH -s $spinners
    
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
