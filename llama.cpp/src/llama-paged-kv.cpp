#include "llama-paged-kv.h"

#include "ggml.h"
#include "ggml-cpp.h"
#include "llama-model.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

void BlockPool::init(
        ggml_backend_buffer_type_t buft,
        uint32_t n_blocks,
        uint32_t n_layer,
        uint32_t n_embd_k,
        uint32_t n_embd_v,
        ggml_type type_k,
        ggml_type type_v,
        ggml_context* ctx) {

    buft_ = buft;
    n_blocks_ = n_blocks;
    n_layer_ = n_layer;
    n_embd_k_ = n_embd_k;
    n_embd_v_ = n_embd_v;
    type_k_ = type_k;
    type_v_ = type_v;
    ctx_ = ctx;

    // Calculate total memory needed for ALL blocks (like vLLM's block table)
    // Each block has KV_BLOCK_SIZE tokens capacity
    // K and V for each layer
    size_t k_block_size = (size_t)n_embd_k * KV_BLOCK_SIZE * ggml_type_size(type_k);
    size_t v_block_size = (size_t)n_embd_v * KV_BLOCK_SIZE * ggml_type_size(type_v);
    size_t per_block_total = k_block_size + v_block_size;
    size_t total_pool_size = (size_t)n_blocks * per_block_total;

    fprintf(stderr, "=== BlockPool: Allocating %zu bytes (%.1f MB) for %u blocks ===\n",
            total_pool_size, total_pool_size / 1024.0 / 1024.0, n_blocks);
    fprintf(stderr, "    Per block: K=%zu + V=%zu = %zu bytes (%.1f KB)\n",
            k_block_size, v_block_size, per_block_total, per_block_total / 1024.0);

    // Allocate ONE big GPU buffer for ALL blocks (vLLM style!)
    if (buft && total_pool_size > 0) {
        pool_buffer_.reset(ggml_backend_buft_alloc_buffer(buft, total_pool_size));
        if (!pool_buffer_) {
            fprintf(stderr, "WARNING: failed to allocate page pool buffer\n");
        } else {
            fprintf(stderr, "    Pool buffer allocated on device\n");
        }
    }

    // Prepare block structures
    blocks_.resize(n_blocks);

    for (uint32_t i = 0; i < n_blocks; ++i) {
        KVBlock& block = blocks_[i];
        block.block_id = i;
        block.n_tokens = 0;
        block.is_free = true;
        block.ref_count = 0;
        
        block.k_tensors.resize(n_layer);
        block.v_tensors.resize(n_layer);

        for (uint32_t l = 0; l < n_layer; ++l) {
            block.k_tensors[l] = nullptr;
            block.v_tensors[l] = nullptr;
        }

        free_blocks_.push_back(&block);
    }

    is_lazy_init_ = false;
}

KVBlock* BlockPool::allocate() const {
    if (free_blocks_.empty()) {
        return nullptr;
    }

    KVBlock* block = free_blocks_.back();
    free_blocks_.pop_back();
    block->is_free = false;
    block->n_tokens = 0;

    if (!pool_buffer_ || !ctx_) {
        // No pool buffer, tensors will be allocated lazily
        return block;
    }

    // Calculate base offset for this block in the pool
    size_t k_block_size = (size_t)n_embd_k_ * KV_BLOCK_SIZE * ggml_type_size(type_k_);
    size_t v_block_size = (size_t)n_embd_v_ * KV_BLOCK_SIZE * ggml_type_size(type_v_);
    size_t block_total = k_block_size + v_block_size;
    size_t block_base = (size_t)block->block_id * block_total;

    for (uint32_t l = 0; l < n_layer_; ++l) {
        // Create tensor views into pool buffer
        // View offset for K tensor in this layer
        size_t k_offset = block_base;

        // View offset for V tensor in this layer
        size_t v_offset = block_base + k_block_size;

        char name[64];
        snprintf(name, sizeof(name), "paged_k_l%d_b%d", l, block->block_id);

        block->k_tensors[l] = ggml_view_tensor(ctx_, nullptr);
        if (block->k_tensors[l]) {
            ggml_format_name(block->k_tensors[l], name);
            block->k_tensors[l]->buffer = pool_buffer_.get();
            block->k_tensors[l]->data = (char*)ggml_backend_buffer_get_base(pool_buffer_.get()) + k_offset;
            block->k_tensors[l]->ne[0] = n_embd_k_;
            block->k_tensors[l]->ne[1] = KV_BLOCK_SIZE;
            block->k_tensors[l]->nb[0] = ggml_type_size(type_k_);
            block->k_tensors[l]->nb[1] = block->k_tensors[l]->nb[0] * (n_embd_k_ / ggml_blck_size(type_k_));
        }

        snprintf(name, sizeof(name), "paged_v_l%d_b%d", l, block->block_id);
        block->v_tensors[l] = ggml_view_tensor(ctx_, nullptr);
        if (block->v_tensors[l]) {
            ggml_format_name(block->v_tensors[l], name);
            block->v_tensors[l]->buffer = pool_buffer_.get();
            block->v_tensors[l]->data = (char*)ggml_backend_buffer_get_base(pool_buffer_.get()) + v_offset;
            block->v_tensors[l]->ne[0] = n_embd_v_;
            block->v_tensors[l]->ne[1] = KV_BLOCK_SIZE;
            block->v_tensors[l]->nb[0] = ggml_type_size(type_v_);
            block->v_tensors[l]->nb[1] = block->v_tensors[l]->nb[0] * (n_embd_v_ / ggml_blck_size(type_v_));
        }
    }

    return block;
}

void BlockPool::release(KVBlock* block) const {
    if (!block) return;

    block->n_tokens = 0;
    block->is_free = true;
    block->ref_count = 0;

    free_blocks_.push_back(block);
}

void BlockPool::write_tokens(KVBlock* block, int layer, const void* k_data, const void* v_data, int n) const {
    if (!block || !k_data || layer < 0 || layer >= (int)n_layer_) return;

    const size_t k_elem_size = ggml_type_size(type_k_);
    const size_t v_elem_size = ggml_type_size(type_v_);

    const int offset = block->n_tokens;
    const size_t k_offset = (size_t)offset * n_embd_k_ * k_elem_size;
    const size_t v_offset = (size_t)offset * n_embd_v_ * v_elem_size;

    const size_t k_size = (size_t)n_embd_k_ * n * k_elem_size;
    const size_t v_size = (size_t)n_embd_v_ * n * v_elem_size;

    ggml_tensor* k_tensor = block->k_tensors[layer];
    ggml_tensor* v_tensor = block->v_tensors[layer];

    if (k_tensor && k_data) {
        ggml_backend_tensor_set(k_tensor, k_data, k_offset, k_size);
    }
    if (v_tensor && v_data) {
        ggml_backend_tensor_set(v_tensor, v_data, v_offset, v_size);
    }

    block->n_tokens += n;
}

bool BlockPool::allocate_tensor(KVBlock* block, int layer, ggml_context* ctx, ggml_tensor* src_k, ggml_tensor* src_v) const {
    if (!block || layer < 0 || layer >= (int)n_layer_) return false;
    if (!ctx) return false;

    char name[64];

    // Create K tensor - share buffer from source
    if (src_k) {
        snprintf(name, sizeof(name), "paged_k_l%d_b%d", layer, block->block_id);
        ggml_tensor* k = ggml_view_2d(ctx, src_k, src_k->ne[0], KV_BLOCK_SIZE, src_k->nb[1], 0);
        ggml_format_name(k, name);
        // Copy buffer backend from source to maintain GPU allocation
        k->buffer = src_k->buffer;
        block->k_tensors[layer] = k;
    }

    // Create V tensor - share buffer from source
    if (src_v) {
        snprintf(name, sizeof(name), "paged_v_l%d_b%d", layer, block->block_id);
        ggml_tensor* v = ggml_view_2d(ctx, src_v, src_v->ne[0], KV_BLOCK_SIZE, src_v->nb[1], 0);
        ggml_format_name(v, name);
        v->buffer = src_v->buffer;
        block->v_tensors[layer] = v;
    }

    return true;
}

void BlockPool::free_tensor(KVBlock* block, int layer) const {
    if (!block || layer < 0 || layer >= (int)n_layer_) return;
    block->k_tensors[layer] = nullptr;
    block->v_tensors[layer] = nullptr;
}

void SeqBlockTable::allocate(int n_tokens) {
    mappings.clear();
    total_tokens = n_tokens;
}

void SeqBlockTable::append_block(KVBlock* block, int n) {
    if (!block || n <= 0) return;

    BlockMapping mapping;
    mapping.block = block;
    mapping.offset = block->n_tokens;
    mapping.n_tokens = n;

    mappings.push_back(mapping);
    total_tokens += n;

    block->n_tokens += n;
}

void SeqBlockTable::clear() {
    mappings.clear();
    total_tokens = 0;
}

const BlockMapping* SeqBlockTable::get_block_at(int pos) const {
    if (pos < 0 || pos >= total_tokens || mappings.empty()) {
        return nullptr;
    }

    int block_idx = pos / KV_BLOCK_SIZE;
    if (block_idx >= (int)mappings.size()) {
        return nullptr;
    }

    return &mappings[block_idx];
}

void PagedKVCache::init(
        const llama_model& model,
        ggml_type type_k,
        ggml_type type_v,
        bool offload,
        uint32_t kv_size,
        uint32_t n_seq_max,
        ggml_context* ctx) {

    type_k_ = type_k;
    type_v_ = type_v;
    offload_ = offload;
    kv_size_ = kv_size;
    n_seq_max_ = n_seq_max;

    n_layer_ = model.hparams.n_layer;

    uint32_t n_blocks = (kv_size + KV_BLOCK_SIZE - 1) / KV_BLOCK_SIZE;

    const char* kv_blocks_env = getenv("LLAMA_KV_BLOCKS");
    if (kv_blocks_env) {
        int n = atoi(kv_blocks_env);
        if (n > 0 && n < (int)n_blocks) {
            fprintf(stderr, "=== PagedKVCache: Limiting blocks from %u to %d ===\n", n_blocks, n);
            n_blocks = (uint32_t)n;
        }
    }

    ggml_backend_buffer_type_t buft = ggml_backend_cpu_buffer_type();

    if (offload) {
        auto* dev = model.dev_layer(0);
        buft = ggml_backend_dev_buffer_type(dev);
    }

    const uint32_t n_embd_k = model.hparams.n_embd_head_k() * model.hparams.n_head_kv();
    const uint32_t n_embd_v = model.hparams.n_embd_head_v() * model.hparams.n_head_kv();

    pool_.init(buft, n_blocks, n_layer_, n_embd_k, n_embd_v, type_k, type_v, ctx);
}

void PagedKVCache::clear(bool data) {
    (void)data;
    seq_tables_.clear();
    seq_order_.clear();
    prefix_cache_.clear();
}

static uint64_t compute_prompt_hash(const int32_t* tokens, int n_tokens) {
    uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
    for (int i = 0; i < n_tokens; ++i) {
        hash ^= (uint64_t)tokens[i];
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

void PagedKVCache::register_prefix(uint32_t seq_id, const int32_t* tokens, int n_tokens) {
    if (!tokens || n_tokens <= 0) return;
    
    uint64_t hash = compute_prompt_hash(tokens, n_tokens);
    PrefixEntry entry;
    entry.seq_id = seq_id;
    entry.n_tokens = n_tokens;
    entry.hash = hash;
    
    prefix_cache_[hash] = entry;
    fprintf(stderr, "=== PagedKVCache: Registered prefix cache hash=%llu seq=%u tokens=%d ===\n", 
            (unsigned long long)hash, seq_id, n_tokens);
}

int PagedKVCache::find_slot(uint32_t seq_id, int n_tokens) const {
    // Check prefix cache (for prompt phase)
    // This is called before tokens are written, so we check if there's a matching prefix
    // Note: Currently we don't have access to tokens here, so prefix cache works at sequence level
    // The actual prefix matching happens when registering
    
    SeqBlockTable* table = nullptr;

    auto it = seq_tables_.find(seq_id);
    if (it == seq_tables_.end()) {
        table = &seq_tables_[seq_id];
        table->set_seq_id(seq_id);
        seq_order_.push_back(seq_id);
    } else {
        table = &it->second;
    }

    // LRU eviction: if no free blocks, evict oldest sequence
    while (pool_.get_free_count() == 0 && !seq_order_.empty()) {
        uint32_t oldest_seq = seq_order_.front();
        if (oldest_seq == seq_id) {
            // Can't evict self
            break;
        }
        seq_tables_.erase(oldest_seq);
        seq_order_.erase(seq_order_.begin());
        fprintf(stderr, "=== PagedKVCache: Evicted seq %u (LRU) ===\n", oldest_seq);
    }

    int needed_blocks = (n_tokens + KV_BLOCK_SIZE - 1) / KV_BLOCK_SIZE;

// Allocate only what's needed (not full blocks)
    int tokens_per_block = n_tokens;  // First block gets remaining tokens
    
    for (int i = 0; i < needed_blocks; ++i) {
        KVBlock* block = pool_.allocate();
        if (!block) {
            return -1;
        }

        // First block: partial if n_tokens < KV_BLOCK_SIZE
        // Remaining blocks: full blocks
        int tokens_to_fill = (tokens_per_block > KV_BLOCK_SIZE) ? KV_BLOCK_SIZE : tokens_per_block;
        table->append_block(block, tokens_to_fill);
        tokens_per_block -= KV_BLOCK_SIZE;
    }

    // Move to end of LRU order (most recently used)
    auto it_order = std::find(seq_order_.begin(), seq_order_.end(), seq_id);
    if (it_order != seq_order_.end()) {
        seq_order_.erase(it_order);
    }
    seq_order_.push_back(seq_id);

    return 0;
}

void PagedKVCache::write_tokens(uint32_t seq_id, ggml_tensor* k_cur, ggml_tensor* v_cur, int n_tokens) const {
    // Simple version - just record that tokens were processed
    // Actual data remains in original cache
    auto it = seq_tables_.find(seq_id);
    if (it == seq_tables_.end()) {
        return;
    }

    SeqBlockTable& table = it->second;

    const int block_count = table.block_count();
    if (block_count == 0) return;

    // Just track that we've "written" tokens
    for (int bi = 0; bi < block_count; ++bi) {
        KVBlock* block = table.mappings[bi].block;
        block->n_tokens = n_tokens;
    }
}

void PagedKVCache::sync_blocks(int src_stream, int dst_stream) const {
    GGML_UNUSED(src_stream);
    GGML_UNUSED(dst_stream);
}

ggml_tensor* PagedKVCache::gather_k_view(ggml_context* ctx, ggml_tensor* src_k, uint32_t seq_id, int n_tokens) const {
    const SeqBlockTable* table = get_seq_table(seq_id);
    if (!table || !ctx || n_tokens <= 0) {
        return nullptr;
    }

    const int n_embd_k = pool_.n_embd_k();
    const ggml_type type_k = pool_.type_k();

    // Fast path: if tokens fit in a single block, return view directly (no concat!)
    if (table->block_count() == 1) {
        const auto& mapping = table->get_blocks()[0];
        ggml_tensor* block_k = mapping.block->get_k(0);
        if (!block_k) return nullptr;

        return ggml_view_2d(ctx, block_k, n_embd_k, n_tokens, block_k->nb[1],
            mapping.offset * n_embd_k * ggml_type_size(type_k));
    }

    // Slow path: need concat for multiple blocks
    int offset = 0;
    ggml_tensor* result = nullptr;

    for (const auto& mapping : table->get_blocks()) {
        KVBlock* block = mapping.block;
        int block_offset = mapping.offset;
        int to_use = std::min(mapping.n_tokens, n_tokens - offset);

        if (to_use <= 0) break;

        ggml_tensor* block_k = block->get_k(0);
        if (!block_k) {
            return nullptr;
        }

        ggml_tensor* view = ggml_view_2d(ctx, block_k, n_embd_k, to_use, block_k->nb[1],
            block_offset * n_embd_k * ggml_type_size(type_k));

        if (offset == 0) {
            result = view;
        } else {
            ggml_tensor* concat = ggml_concat(ctx, result, view, 1);
            result = concat;
        }
        offset += to_use;
    }

    return result;
}

ggml_tensor* PagedKVCache::gather_v_view(ggml_context* ctx, ggml_tensor* src_v, uint32_t seq_id, int n_tokens) const {
    const SeqBlockTable* table = get_seq_table(seq_id);
    if (!table || !ctx || n_tokens <= 0) {
        return nullptr;
    }

    const int n_embd_v = pool_.n_embd_v();
    const ggml_type type_v = pool_.type_v();

    // Fast path: if tokens fit in a single block, return view directly (no concat!)
    if (table->block_count() == 1) {
        const auto& mapping = table->get_blocks()[0];
        ggml_tensor* block_v = mapping.block->get_v(0);
        if (!block_v) return nullptr;

        return ggml_view_2d(ctx, block_v, n_embd_v, n_tokens, block_v->nb[1],
            mapping.offset * n_embd_v * ggml_type_size(type_v));
    }

    // Slow path: need concat for multiple blocks
    int offset = 0;
    ggml_tensor* result = nullptr;

    for (const auto& mapping : table->get_blocks()) {
        KVBlock* block = mapping.block;
        int block_offset = mapping.offset;
        int to_use = std::min(mapping.n_tokens, n_tokens - offset);

        if (to_use <= 0) break;

        ggml_tensor* block_v = block->get_v(0);
        if (!block_v) {
            return nullptr;
        }

        ggml_tensor* view = ggml_view_2d(ctx, block_v, n_embd_v, to_use, block_v->nb[1],
            block_offset * n_embd_v * ggml_type_size(type_v));

        if (offset == 0) {
            result = view;
        } else {
            ggml_tensor* concat = ggml_concat(ctx, result, view, 1);
            result = concat;
        }
        offset += to_use;
    }

    return result;
}

ggml_tensor* PagedKVCache::gather_k(ggml_context* ctx, uint32_t seq_id, int n_tokens) const {
    const SeqBlockTable* table = get_seq_table(seq_id);
    if (!table || !ctx || n_tokens <= 0) {
        return nullptr;
    }

    const uint32_t n_embd_k = pool_.n_embd_k();
    const ggml_type type_k = pool_.type_k();

    ggml_tensor* result = ggml_new_tensor_2d(ctx, type_k, n_embd_k, n_tokens);
    ggml_format_name(result, "gather_k_seq%d", seq_id);

    int offset = 0;
    for (const auto& mapping : table->get_blocks()) {
        KVBlock* block = mapping.block;
        int block_offset = mapping.offset;
        int to_copy = std::min(mapping.n_tokens, n_tokens - offset);

        if (to_copy <= 0) break;

        for (int layer = 0; layer < pool_.n_layer(); ++layer) {
            ggml_tensor* src_k = block->get_k(layer);
            if (!src_k) continue;

            ggml_tensor* view = ggml_view_2d(ctx, src_k, n_embd_k, to_copy, src_k->nb[1], block_offset * n_embd_k * ggml_type_size(type_k));
            ggml_tensor* dst_view = ggml_view_2d(ctx, result, n_embd_k, to_copy, result->nb[1], offset * n_embd_k * ggml_type_size(type_k));

            ggml_tensor* copy = ggml_cpy(ctx, view, dst_view);
            (void)copy;
        }
        offset += to_copy;
    }

    return result;
}

ggml_tensor* PagedKVCache::gather_v(ggml_context* ctx, uint32_t seq_id, int n_tokens) const {
    const SeqBlockTable* table = get_seq_table(seq_id);
    if (!table || !ctx || n_tokens <= 0) {
        return nullptr;
    }

    const uint32_t n_embd_v = pool_.n_embd_v();
    const ggml_type type_v = pool_.type_v();

    ggml_tensor* result = ggml_new_tensor_2d(ctx, type_v, n_embd_v, n_tokens);
    ggml_format_name(result, "gather_v_seq%d", seq_id);

    int offset = 0;
    for (const auto& mapping : table->get_blocks()) {
        KVBlock* block = mapping.block;
        int block_offset = mapping.offset;
        int to_copy = std::min(mapping.n_tokens, n_tokens - offset);

        if (to_copy <= 0) break;

        for (int layer = 0; layer < pool_.n_layer(); ++layer) {
            ggml_tensor* src_v = block->get_v(layer);
            if (!src_v) continue;

            ggml_tensor* view = ggml_view_2d(ctx, src_v, n_embd_v, to_copy, src_v->nb[1], block_offset * n_embd_v * ggml_type_size(type_v));
            ggml_tensor* dst_view = ggml_view_2d(ctx, result, n_embd_v, to_copy, result->nb[1], offset * n_embd_v * ggml_type_size(type_v));

            ggml_tensor* copy = ggml_cpy(ctx, view, dst_view);
            (void)copy;
        }
        offset += to_copy;
    }

    return result;
}

void PagedKVCache::defrag() const {
}

const SeqBlockTable* PagedKVCache::get_seq_table(uint32_t seq_id) const {
    auto it = seq_tables_.find(seq_id);
    if (it == seq_tables_.end()) {
        return nullptr;
    }
    return &it->second;
}

const BlockMapping* PagedKVCache::get_block_at(uint32_t seq_id, int pos) const {
    const SeqBlockTable* table = get_seq_table(seq_id);
    if (!table) {
        return nullptr;
    }
    return table->get_block_at(pos);
}