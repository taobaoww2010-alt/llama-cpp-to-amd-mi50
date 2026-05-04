#!/usr/bin/env python3
"""
Automatic Tensor Split Optimizer
Finds optimal -ts ratio for dual-GPU inference
"""

import subprocess
import re
import time

def run_inference(ts_ratio, n_tokens=30):
    """Run inference with given tensor split ratio"""
    cmd = [
        "./build/bin/llama-cli",
        "-m", "/home/tomlee/models/Qwen2.5-7B-Q3_K_M.gguf",
        "-p", "Write a story:",
        "-n", str(n_tokens),
        "-ngl", "28",
        "-sm", "tensor",
        "-ts", ts_ratio,
        "-v", "0"  # minimal output
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        output = result.stderr + result.stdout
        
        # Extract generation speed
        match = re.search(r"Generation:\s*([\d.]+)", output)
        if match:
            return float(match.group(1))
    except Exception as e:
        print(f"Error: {e}")
    return None

def grid_search():
    """Grid search for optimal split ratio"""
    ratios = []
    for i in range(11):
        for j in range(11 - i):
            if i + j > 0:
                ratios.append(f"{i/10},{j/10}")
    
    results = []
    print("=== Grid Search for Optimal Tensor Split ===")
    print(f"Testing {len(ratios)} ratios...")
    
    for ratio in ratios[:15]:  # Test first 15 for quick results
        speed = run_inference(ratio)
        if speed:
            results.append((ratio, speed))
            print(f"  ts={ratio}: {speed:.1f} t/s")
        time.sleep(0.5)
    
    # Sort by speed
    results.sort(key=lambda x: x[1], reverse=True)
    
    print("\n=== Top 3 Configurations ===")
    for ratio, speed in results[:3]:
        print(f"  {ratio}: {speed:.1f} t/s")
    
    return results[0] if results else None

if __name__ == "__main__":
    best = grid_search()
    if best:
        print(f"\nRecommended: -ts {best[0]} ({best[1]:.1f} t/s)")
