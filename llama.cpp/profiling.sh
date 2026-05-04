#!/bin/bash
# Layer-wise Profiling Script for Radeon VII
# Measures inference time per layer to build cost model

MODEL="/home/tomlee/models/Qwen2.5-7B-Q3_K_M.gguf"
OUTPUT="layer_profiling_results.txt"

echo "=== Layer Profiling for Cost Model ===" | tee $OUTPUT
echo "Model: $MODEL" | tee -a $OUTPUT
echo "" | tee -a $OUTPUT

# Test different layer offloading configurations
for ngl in 0 7 14 21 28; do
    echo "--- Testing with -ngl $ngl ---" | tee -a $OUTPUT
    
    # Run inference and capture timing
    result=$(./build/bin/llama-cli -m $MODEL -p "Hello" -n 20 -ngl $ngl 2>&1 | grep -E "t/s")
    
    echo "NGL=$ngl: $result" | tee -a $OUTPUT
    
    # Check GPU memory usage
    rocm-smi --query-gpu=memory.used --format=csv | tee -a $OUTPUT
    
    sleep 1
done

echo ""
echo "=== Profiling Complete ==="
echo "Results saved to: $OUTPUT"