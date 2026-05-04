# Llama.cpp 双卡研发对比报告

## 研发背景对比

### 上次研发 (2024-04-16 ~ 2024-04-20)
- **目标**: 在 AMD MI50 (gfx906) 双 GPU 上实现张量并行
- **结果**: 成功实现 Layer 并行模式 (~44 tokens/s)
- **模型**: Qwen2.5-7B-Q3_K_M.gguf

### 本次研发 (2026-04-28)
- **目标**: 继续完善张量并行 + KV 量化支持
- **结果**: Row 模式可运行，但 tensor 模式崩溃
- **模型**: Qwen3.6-27B-UD-Q4_K_XL.gguf

---

## 上次研发解决的问题

| 问题 | 解决方案 | 状态 |
|------|---------|------|
| clang 18 不支持 gfx906 | 添加 C++ include 路径 | ✅ |
| 缺少 C++ 标准库头文件 | `-I/usr/include/c++/11` | ✅ |
| RCCL 死锁 | 添加 `GGML_RCCL_DISABLE=1` | ✅ |
| Ring AllReduce 实现 | 自定义 ggml-tensor-parallel 模块 | ✅ |
| Layer 分割模式 | `--split-mode layer` | ✅ |

### 关键代码修改
- 新增 `ggml-tensor-parallel/` 模块
- 重写通信原语 (Ring AllReduce)
- Layer 级别权重分割

---

## 本次研发解决的问题

| 问题 | 尝试方案 | 状态 |
|------|---------|------|
| FP16 @ 16K context | 直接使用 | ✅ |
| tensor 模式崩溃 | 移除 ggml-backend-meta.cpp 断言 | ❌ |
| KV 量化 + TP | 对齐 block size | ❌ |
| llama-server 退出 | row 模式代替 tensor | ⚠️ |

### 本次代码修改
1. **llama-context.cpp**: 移除 TP+KV quant 阻断
2. **llama-kv-cache.cpp**: KV 命名
3. **ggml-backend-meta.cpp**: 移除 ~30+ 断言
4. **ggml-cuda.h**: 编译修复

---

## 当前状态

### 可用配置
```bash
# Row 模式 (可用)
./bin/llama-server -m model.gguf -ngl 99 -sm row --port 8080

# 性能:
# - 上下文: ~198K tokens
# - 速度: ~20 tokens/s
# - 显存: GPU0 14.6GB, GPU1 15.1GB
```

### 不可用配置
- ❌ tensor 模式 (meta backend 崩溃)
- ❌ KV 量化 + TP

---

## 问题分析

### tensor 模式问题
llama-server 使用 meta backend 进行张量分配，触发大量断言:
1. `handle_set_rows` - SPLIT_AXIS_1 检查
2. `get_split_state` - 维度计算
3. `buffer_init_tensor` - 内存分配

### 根本原因
- llama-cli 直接调用 CUDA，不经过 meta backend
- llama-server 使用 meta backend 统一管理多设备内存
- TP + 量化组合在 meta backend 中未完全实现

### Row 模式区别
- Row 模式: 权重按行分割，KV 在同一 GPU
- Tensor 模式: 权重和 KV 都跨卡分割 (需要 meta backend)

---

## API 配置 (当前可用)

### Base URL
```
http://192.168.31.181:8080
```

### OpenAI 兼容 API
```bash
# Chat Completions
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "Hello"}],
    "max_tokens": 50
  }'

# Models
curl http://localhost:8080/v1/models
```

### 参数说明
| 参数 | 值 | 说明 |
|------|-----|------|
| `-ngl 99` | 99 | 所有层到 GPU |
| `-sm row` | row | Row 分割模式 |
| `--port 8080` | 8080 | 服务端口 |
| `--parallel 1` | 1 | 并发数 |

---

## 后续建议

### 短期 (保守方案)
- 继续使用 row 模式
- 限制并发为 1

### 长期 (需深入开发)
- 重新实现 ggml-backend-meta.cpp 对 TP 的支持
- 添加量化 block 对齐支持
- 修复 tensor split state 计算

---

## 对比总结

| 维度 | 上次 | 本次 |
|------|-----|------|
| 编译 | ✅ 完成 | ✅ 完成 |
| Layer 并行 | ✅ 44 t/s | ✅ 可用 |
| Tensor 并行 | ❌ 未实现 | ❌ 崩溃 |
| KV 量化 | ⚠️ 未测试 | ❌ 断言 |
| Row 模式 | ⚠️ 乱码 | ✅ 可用 |
| llama-server | ✅ 可用 | ⚠️ 需 row 模式 |

**结论**: 上次研发解决了编译和 Layer 并行问题。本次尝试 Tensor 并行和 KV 量化失败，但验证了 Row 模式可用于生产。