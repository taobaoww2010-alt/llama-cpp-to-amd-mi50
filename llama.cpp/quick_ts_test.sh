#!/bin/bash
# Quick Tensor Split Ratio Tester
# Tests common ratios and finds optimal

MODEL="/home/tomlee/models/Qwen2.5-7B-Q3_K_M.gguf"
BIN="./build/bin/llama-cli"

echo "=== Tensor Split Ratio Auto-Test ==="

for ratio in "1,1" "0.9,0.1" "0.8,0.2" "0.7,0.3" "0.6,0.4" "0.5,0.5" "0.4,0.6" "0.3,0.7" "0.2,0.8" "0.1,0.9"; do
    echo -n "ts=$ratio: "
    timeout 10 $BIN -m $MODEL -p "Test" -n 20 -ngl 28 -sm tensor -ts $ratio 2>&1 | grep -oP "Generation: \K[\d.]+" || echo "fail"
done | sort -t: -k2 -rn

echo ""
echo "Best ratio is at top (highest t/s)"