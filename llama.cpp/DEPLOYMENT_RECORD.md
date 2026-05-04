# 预发环境部署记录

## 版本
**v1.0** - 2026-05-02

## 服务器信息
- IP: 192.168.31.164
- GPU: 2x AMD Radeon Pro VII (32GB VRAM each)
- ROCm: 6.3.0

## 模型配置
- 模型: Qwen3.6-27B-UD-Q4_K_XL.gguf
- Context: 98K (98304)
- 量化: Q4_K_XL
- API Key: not-needed

## 当前启动命令
```bash
LLAMA_USE_PAGED_KV=1 LLAMA_KV_BLOCKS=512 ./build/bin/llama-server \
  -m /home/tomlee/models/Qwen3.6-27B-UD-Q4_K_XL.gguf \
  -c 98304 --batch-size 512 --threads 8 --threads-batch 4 --temp 0.6 --repeat-penalty 1.05 \
  --context-shift --cache-idle-slots -ngl 999 --host 0.0.0.0 --port 8080 \
  --api-key not-needed
```

## 核心参数说明
| 参数 | 值 | 说明 |
|------|-----|------|
| -c | 98304 | Context 98K |
| --context-shift | 启用 | KV 滚动缓存 |
| --cache-idle-slots | 启用 | 空闲时保存 slot |
| --api-key | not-needed | API 认证 |
| --host 0.0.0.0 | 监听 | 允许远程访问 |

## VRAM 基线 (2026-05-02 19:42)
| GPU | VRAM 使用 |
|-----|----------|
| GPU 0 | 76% |
| GPU 1 | 78% |

### 内存分布 (启动时)
- Model: 16.39 GB (GPU0: 7.76GB + GPU1: 8.34GB)
- KV Cache: 6GB (3GB each GPU)
- Compute: ~2GB
- Host: 0.8GB
- **总计: ~25GB**

## 监控计划
### 检查命令
```bash
rocm-smi
# 或
watch -n 1 rocm-smi
```

### 关注指标
1. **VRAM 百分比** - 是否持续增长
2. **GPU 利用率** - 是否异常
3. **温度** - 是否过热

### 如果 VRAM 持续增长，可能原因
1. **泄漏**: 代码问题（我们的修改）
2. **缓存积累**: prompt cache 正常行为
3. **context-shift 失效**: KV 滚动不工作
4. **碎片化**: KV cache 碎片

### 预期行为
- 稳定运行时 VRAM 应保持稳定
- 长时间对话可能因 context-shift 略有波动
- 不应持续增长超过 5%

## API 端点
- 本地: http://localhost:8080/v1
- 远程: http://192.168.31.164:8080/v1

## 性能表现
- 生成速度: ~19-20 t/s
- GPU 负载: 动态均衡
- 温度: <71°C（满载）

## 部署日期
2026-05-02

## 更新记录
| 日期 | VRAM GPU0 | VRAM GPU1 | 备注 |
|------|-----------|-----------|------|
| 2026-05-02 19:42 | 76% | 78% | 初始状态 |