# PagedAttention Implementation - Version History

## Version 1.0 - 2026-05-02 (Initial Release)

### Deployment Status
- **Production Ready**: Dual-write approach (v1.0-paged-stable)
- **Context**: 98K tokens
- **API Key**: not-needed
- **Endpoint**: http://192.168.31.164:8080/v1

### Implemented Features

#### 1. BlockPool Physical Memory Allocation
- **File**: `src/llama-paged-kv.cpp` (NEW)
- Configurable size via LLAMA_KV_BLOCKS (default 512, range 128-4096)
- Per-block: K=32KB + V=32KB = 64KB
- Memory calculated from n_embd, n_head, kv_quant_type
- Works with Q4_K_XL (32K block) and Q6_K (48K block)

#### 2. Dual-Write KV Cache
- **File**: `src/llama-kv-cache.cpp` (MODIFIED)
- cpy_k/cpy_v write to BlockPool AND original KV cache
- get_k/get_v read from BlockPool when available
- Falls back to original KV for multi-device mode

#### 3. Prefix Cache C API
- **File**: `include/llama.h` (MODIFIED)
- llama_memory_register_prefix() function added
- Server calls this at prefix detection (tools/server/server-context.cpp)

#### 4. Single-Sequence Fast Path
- **File**: `src/llama-kv-cache.cpp` (MODIFIED)
- find_slot optimized for single sequence (direct index calculation)
- Multi-seq uses iterative search fallback

#### 5. Paged Gather for get_k/get_v
- **File**: `src/llama-kv-cache.cpp` (MODIFIED)
- Single-block: direct offset calculation
- Multi-block: gather all blocks

### Performance
- Single-seq: ~21 t/s (matches non-paged baseline)
- 2-concurrent: ~14+ t/s stable
- Memory overhead: ~32MB for BlockPool

### Known Limitations
- Multi-device (tensor-split): Falls back to original KV cache
- Pure paged mode: Disabled due to graph-building complexity
- Beta API (structured output): Not supported

### Environment Variables
| Variable | Default | Description |
|----------|---------|-------------|
| LLAMA_USE_PAGED_KV | 0 | Enable paged mode |
| LLAMA_KV_BLOCKS | 512 | Number of KV blocks in pool |
| LLAMA_KV_QUANT | (none) | Quantization type (8=Q8_0) |

### VRAM Usage Baseline (2026-05-02)
- GPU 0: 76%
- GPU 1: 78%
- Total: ~25GB / 32GB

### Modified Files Summary
```
ggml/include/ggml.h              - ggml_paged_kv_desc structure
ggml/src/ggml.c                  - ggml_flash_attn_ext with paged_desc
include/llama.h                  - llama_memory_register_prefix() API
src/llama-kv-cache.cpp            - Dual-write, find_slot, get_k/get_v
src/llama-kv-cache.h              - BlockPool integration
src/llama-graph.cpp               - Paged mode detection
src/llama-context.cpp             - Prefix cache integration
tools/server/server-context.cpp   - register_prefix call
```

### Documentation Files
- `PAGED_ATTENTION_PROGRESS.md` - Development notes
- `DEPLOYMENT_RECORD.md` - Deployment guide
- `llama.service` - systemd service file

---

## Changelog

### 2026-05-02
- Initial release v1.0
- 98K context configured
- API key support added
- Production deployment completed

### 2026-04-29
- BlockPool implementation complete
- Dual-write approach finalized
- Multi-device fallback implemented

### 2026-04-28
- Initial PagedAttention exploration started