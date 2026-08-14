#!/bin/bash
# scripts/embed_web_ui.sh
# 将前端构建产物 dist/ 嵌入为 C 数组（安全拼接，无语法错误）
# 用法：./scripts/embed_web_ui.sh [dist_dir] [output_c] [output_h]

set -e

DIST_DIR="${1:-dist}"
OUTPUT_C="${2:-src/core/web_ui_data.c}"
OUTPUT_H="${3:-src/core/web_ui_data.h}"

if [ ! -d "$DIST_DIR" ]; then
    echo "Error: dist directory '$DIST_DIR' not found." >&2
    echo "Usage: $0 [dist_dir] [output_c] [output_h]" >&2
    exit 1
fi

echo "Embedding web UI from $DIST_DIR ..."

# 创建临时目录
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# 1. 收集所有文件（按名称排序）
find "$DIST_DIR" -type f | sort > "$TMP_DIR/files.lst"

# 2. 拼接所有文件内容为一个二进制文件
DATA_BIN="$TMP_DIR/all_data.bin"
> "$DATA_BIN"
OFFSET=0
declare -a FILE_NAMES
declare -a FILE_OFFSETS
declare -a FILE_SIZES

while IFS= read -r FILE; do
    REL_PATH="${FILE#$DIST_DIR/}"
    SIZE=$(stat -c%s "$FILE")
    FILE_NAMES+=("$REL_PATH")
    FILE_OFFSETS+=("$OFFSET")
    FILE_SIZES+=("$SIZE")
    cat "$FILE" >> "$DATA_BIN"
    OFFSET=$((OFFSET + SIZE))
done < "$TMP_DIR/files.lst"

# 3. 生成数据块 C 数组（使用 xxd -i）
xxd -i "$DATA_BIN" > "$TMP_DIR/data.c"
# 提取数组内容（去掉声明行和末尾分号）
sed -e '1d' -e '$d' "$TMP_DIR/data.c" > "$TMP_DIR/data_body.c"
# 添加数组定义
cat > "$OUTPUT_C" << 'EOF'
#include "web_ui_data.h"

/* 数据块：所有文件拼接而成 */
const unsigned char web_ui_data[] = {
EOF
cat "$TMP_DIR/data_body.c" >> "$OUTPUT_C"
echo '};' >> "$OUTPUT_C"

# 4. 生成索引表
cat >> "$OUTPUT_C" << 'EOF'

/* 文件索引：名称、偏移、大小 */
const web_ui_file_t web_ui_files[] = {
EOF

COUNT=${#FILE_NAMES[@]}
for ((i=0; i<COUNT; i++)); do
    NAME="${FILE_NAMES[$i]}"
    OFFSET="${FILE_OFFSETS[$i]}"
    SIZE="${FILE_SIZES[$i]}"
    # 如果最后一个元素不加逗号
    if [ $i -eq $((COUNT-1)) ]; then
        echo "    { \"$NAME\", $OFFSET, $SIZE }" >> "$OUTPUT_C"
    else
        echo "    { \"$NAME\", $OFFSET, $SIZE }," >> "$OUTPUT_C"
    fi
done

cat >> "$OUTPUT_C" << 'EOF'
};

const unsigned int web_ui_data_len = sizeof(web_ui_data);
const unsigned int web_ui_file_count = sizeof(web_ui_files) / sizeof(web_ui_files[0]);
EOF

# 5. 生成头文件
cat > "$OUTPUT_H" << 'EOF'
#ifndef CORE_WEB_UI_DATA_H
#define CORE_WEB_UI_DATA_H

#include <stdint.h>

/* 二进制数据块 */
extern const unsigned char web_ui_data[];
extern const unsigned int web_ui_data_len;

/* 文件索引结构 */
typedef struct {
    const char *name;
    unsigned int offset;
    unsigned int size;
} web_ui_file_t;

extern const web_ui_file_t web_ui_files[];
extern const unsigned int web_ui_file_count;

#endif
EOF

echo "Embedded $OFFSET bytes in $OUTPUT_C and $OUTPUT_H"