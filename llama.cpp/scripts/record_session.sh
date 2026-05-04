#!/bin/bash
# 对话记录脚本
# 用法: ./record_session.sh [session_name]

SESSION_NAME=${1:-$(date +%Y%m%d_%H%M%S)}
LOG_FILE="session_logs/${SESSION_NAME}.md"

mkdir -p session_logs

# 获取当前工作目录
echo "# 对话记录 - $SESSION_NAME" > "$LOG_FILE"
echo "时间: $(date)" >> "$LOG_FILE"
echo "" >> "$LOG_FILE"

echo "## 系统信息" >> "$LOG_FILE"
echo "\`- Platform: $(uname -s -m)\`" >> "$LOG_FILE"
echo "\`- Shell: bash\`" >> "$LOG_FILE"
echo "" >> "$LOG_FILE"

echo "## 对话记录" >> "$LOG_FILE"
echo "" >> "$LOG_FILE"

echo "开始记录..."
echo "# Session: $SESSION_NAME" > "$LOG_FILE"
echo "Started: $(date)" >> "$LOG_FILE"

# 等待输入并记录
while true; do
    echo -n "> "
    if ! read line; then
        break
    fi
    echo "User: $line" >> "$LOG_FILE"
    echo "" >> "$LOG_FILE"
    
    # 可以在这里添加处理逻辑
done

echo "Session ended: $(date)" >> "$LOG_FILE"
echo "已保存到 $LOG_FILE"