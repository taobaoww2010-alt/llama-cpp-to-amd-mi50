#pragma once

#include "llama.h"
#include "ggml.h"
#include "ggml-cpp.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

static constexpr int KV_BLOCK_SIZE = 16;

struct KVBlock {
    int block_id = -1;
    int n_tokens = 0;
    int ref_count = 0;
    bool is_free = true;

    std::vector<ggml_tensor*> k_tensors;
    std::vector<ggml_tensor*> v_tensors;

    bool is_full() const { return n_tokens >= (int)KV_BLOCK_SIZE; }
    bool has_space(int n) const { return n_tokens + n <= (int)KV_BLOCK_SIZE; }
    int available() const { return KV_BLOCK_SIZE - n_tokens; }

    ggml_tensor* get_k(int layer) const { return layer < (int)k_tensors.size() ? k_tensors[layer] : nullptr; }
    ggml_tensor* get_v(int layer) const { return layer < (int)v_tensors.size() ? v_tensors[layer] : nullptr; }
};

class BlockPool {
public:
    BlockPool() = default;
    ~BlockPool() = default;

    void init(ggml_backend_buffer_type_t buft, uint32_t n_blocks, uint32_t n_layer, uint32_t n_embd_k, uint32_t n_embd_v, ggml_type type_k, ggml_type type_v, ggml_context* ctx);

    KVBlock* allocate() const;
    void release(KVBlock* block) const;
    int get_free_count() const { return (int)free_blocks_.size(); }
    uint32_t total_blocks() const { return n_blocks_; }

    void write_tokens(KVBlock* block, int layer, const void* k_data, const void* v_data, int n) const;

    bool allocate_tensor(KVBlock* block, int layer, ggml_context* ctx, ggml_tensor* src_k, ggml_tensor* src_v) const;
    void free_tensor(KVBlock* block, int layer) const;

    uint32_t n_embd_k() const { return n_embd_k_; }
    uint32_t n_embd_v() const { return n_embd_v_; }
    uint32_t n_layer() const { return n_layer_; }
    ggml_type type_k() const { return type_k_; }
    ggml_type type_v() const { return type_v_; }
    ggml_context* ctx() const { return ctx_; }

private:
    ggml_backend_buffer_type_t buft_ = nullptr;
    ggml_backend_buffer_ptr pool_buffer_;
    uint32_t n_blocks_ = 0;
    uint32_t n_layer_ = 0;
    uint32_t n_embd_k_ = 0;
    uint32_t n_embd_v_ = 0;
    ggml_type type_k_ = GGML_TYPE_F32;
    ggml_type type_v_ = GGML_TYPE_F32;
    ggml_context* ctx_ = nullptr;
    bool is_lazy_init_ = false;

    mutable std::vector<KVBlock> blocks_;
    mutable std::vector<KVBlock*> free_blocks_;
};

struct BlockMapping {
    KVBlock* block;
    int offset;
    int n_tokens;
};

struct SeqBlockTable {
public:
    uint32_t seq_id;
    std::vector<BlockMapping> mappings;
    int total_tokens;

    SeqBlockTable() : seq_id(0), total_tokens(0) {}
    explicit SeqBlockTable(uint32_t sid) : seq_id(sid), total_tokens(0) {}

    void allocate(int n_tokens);
    void append_block(KVBlock* block, int n);
    void clear();
    void set_seq_id(uint32_t sid) { seq_id = sid; }

    const std::vector<BlockMapping>& get_blocks() const { return mappings; }
    int last_block_available() const;

    bool can_append(int n) const;
    int get_total_tokens() const { return total_tokens; }
    bool is_empty() const { return mappings.empty(); }
    int block_count() const { return (int)mappings.size(); }

    const BlockMapping* get_block_at(int pos) const;
};

inline bool SeqBlockTable::can_append(int n) const {
    if (mappings.empty()) return true;
    const auto& last = mappings.back();
    return last.block->has_space(n);
}

inline int SeqBlockTable::last_block_available() const {
    if (mappings.empty()) return KV_BLOCK_SIZE;
    return mappings.back().block->available();
}

class PagedKVCache {
public:
    PagedKVCache() = default;
    ~PagedKVCache() = default;

    void init(
            const llama_model& model,
            ggml_type type_k,
            ggml_type type_v,
            bool offload,
            uint32_t kv_size,
            uint32_t n_seq_max,
            ggml_context* ctx);

    void clear(bool data);

    int find_slot(uint32_t seq_id, int n_tokens) const;
    
    // Register a prompt for prefix caching
    void register_prefix(uint32_t seq_id, const int32_t* tokens, int n_tokens);

    // Write new K/V tokens to the cache
    void write_tokens(uint32_t seq_id, ggml_tensor* k_cur, ggml_tensor* v_cur, int n_tokens) const;

    // Gather views from existing layer tensors (for attention)
    ggml_tensor* gather_k_view(ggml_context* ctx, ggml_tensor* src_k, uint32_t seq_id, int n_tokens) const;
    ggml_tensor* gather_v_view(ggml_context* ctx, ggml_tensor* src_v, uint32_t seq_id, int n_tokens) const;

    // Original gather (returns new tensors)
    ggml_tensor* gather_k(ggml_context* ctx, uint32_t seq_id, int n_tokens) const;
    ggml_tensor* gather_v(ggml_context* ctx, uint32_t seq_id, int n_tokens) const;

    void sync_blocks(int src_stream, int dst_stream) const;
    void defrag() const;

    const SeqBlockTable* get_seq_table(uint32_t seq_id) const;
    const BlockMapping* get_block_at(uint32_t seq_id, int pos) const;

    BlockPool* get_pool() { return &pool_; }

    uint32_t get_size() const { return kv_size_; }
    int get_free_blocks() const { return pool_.get_free_count(); }
    int get_n_layer() const { return n_layer_; }

private:
    bool is_initialized_ = false;
    BlockPool pool_;
    mutable std::unordered_map<uint32_t, SeqBlockTable> seq_tables_;
    mutable std::vector<uint32_t> seq_order_;

    // Prefix caching: hash of prompt -> (seq_id, n_tokens)
    struct PrefixEntry {
        uint32_t seq_id;
        int n_tokens;
        uint64_t hash;
    };
    mutable std::unordered_map<uint64_t, PrefixEntry> prefix_cache_;

    uint32_t kv_size_ = 0;
    uint32_t n_seq_max_ = 1;
    uint32_t n_layer_ = 0;
    bool offload_ = false;

    ggml_type type_k_ = GGML_TYPE_F32;
    ggml_type type_v_ = GGML_TYPE_F32;
};