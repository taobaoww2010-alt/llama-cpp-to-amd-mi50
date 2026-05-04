# llama.cpp to AMD MI50

基于 llama.cpp 在 AMD GPU (Radeon VII / MI50, gfx906) 上实现大语言模型推理与张量并行优化的开发项目。

## 项目背景

本项目旨在让 llama.cpp 支持双卡 Radeon VII (gfx906) 使用张量并行运行大语言模型，同时探索本地 LLM Agent 的可行性。

## 目录结构

```
/home/tomlee/code/
├── llama.cpp/                    # llama.cpp 主仓库（已合并上游）
│   ├── include/llama.h           # 对外接口
│   ├── src/llama.cpp            # 调度层
│   └── ggml/
│       ├── include/ggml.h       # GGML 头文件
│       └── src/
│           ├── ggml.c           # 核心 C 实现
│           ├── ggml-quants.c    # 量化实现
│           ├── ggml-alloc.c     # 内存分配
│           ├── ggml-backend-meta.cpp  # 张量并行核心
│           └── ggml-cuda/       # HIP/CUDA kernel
├── reference/
│   ├── vllm/                    # vLLM 张量并行参考
│   └── vllm-gfx906/             # gfx906 适配版本
├── rocm/
│   ├── HIP/                     # HIP 底层 API
│   ├── ROCR-Runtime/            # ROCm 运行时
│   └── rocBLAS/                 # 矩阵运算库
├── conversations/                # 开发过程对话记录
└── 研发记录.md                  # 详细研发记录
```

## 环境配置

- **ROCm**: 6.3.0
- **GPU**: 2x Radeon VII (gfx906)
- **ROCm clang**: `/opt/rocm-6.3.0/lib/llvm/bin/clang++`
- **代理**: `http://<user>:<pass>@<proxy-host>:<port>`

## 编译配置

```bash
CC=/usr/bin/gcc CXX=/usr/bin/g++ \
HIPCC=/opt/rocm-6.3.0/lib/llvm/bin/clang++ \
HIP_PATH=/opt/rocm-6.3.0 \
HIP_DEVICE_LIB_PATH=/opt/rocm-6.3.0/lib/llvm/lib/clang/18/lib/amdgcn \
cmake -S . -B build -DGGML_HIP=ON -DGGML_HIP_RCCL=ON -DGPU_TARGETS=gfx906
```

## 关键发现

1. llama.cpp 已内置 meta backend (`ggml-backend-meta.cpp`)
2. 底层通过 NCCL/RCCL 进行跨卡 AllReduce
3. `ggml-cuda` 源码同时适用于 HIP 后端
4. Radeon VII 算力代号: `gfx906`

## 待解决问题

1. ROCm clang 编译配置问题 (libstdc++ 链接)
2. 张量并行通信优化

## 本地 LLM Agent 探索

项目同时评估了 [Hermes Agent](https://hermes-agent.nousresearch.com/) 在本地环境的可行性：

- **系统**: Ubuntu 22.04.5 LTS
- **资源**: 23GB 内存, 44 CPU 核心, 6.8T 硬盘
- **已安装**: Docker, Node.js v22.22.2, Python3, Git

Hermes Agent 支持通过 llama.cpp 本地服务器作为 LLM 后端，配置 OpenAI 兼容接口即可。

## 参考资料

- [llama.cpp 官方仓库](https://github.com/ggerganov/llama.cpp)
- [vLLM 张量并行](https://github.com/vllm-project/vllm)
- [vLLM gfx906 适配](https://github.com/nlzy/vllm-gfx906)
- [ROCm HIP](https://github.com/ROCm/HIP)
- [Hermes Agent 文档](https://hermes-agent.nousresearch.com/docs)

## 开发记录

详细的开发过程和思路记录在 `conversations/` 目录中，包含：
- 本地 Hermes Agent 可行性评估
- llama.cpp 大模型部署提要
- 多序列并发下的 llama_batch 打包与 KV Cache 优化
- code 目录同步到 GitHub 的完整流程

---

*项目创建时间: 2026-04-27*  
*最后更新: 2026-05-04*
