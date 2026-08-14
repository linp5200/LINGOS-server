#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS 错误代码生成工具
版本: LN-B-4.3.0.0
功能: 从 error_codes.json 生成 error_codes.h
用法: python3 gen_error_codes.py [input.json] [output.h]
"""

import json
import sys
import os
from datetime import datetime

def generate_header(json_path, output_path):
    with open(json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    errors = data.get('errors', [])
    if not errors:
        print("Warning: No errors found in JSON", file=sys.stderr)
    
    # 开始生成头文件
    lines = []
    lines.append('/**')
    lines.append(' * @file    error_codes.h')
    lines.append(' * @brief   错误代码枚举（自动生成，请勿手动修改）')
    lines.append(' * @version LN-B-4.3.0.0')
    lines.append(' * @note    生成时间: {}'.format(datetime.now().strftime('%Y-%m-%d %H:%M:%S')))
    lines.append(' */')
    lines.append('')
    lines.append('#ifndef ERROR_CODES_H')
    lines.append('#define ERROR_CODES_H')
    lines.append('')
    lines.append('#include <stdint.h>')
    lines.append('')
    lines.append('/* ============================================================')
    lines.append(' * 错误代码结构')
    lines.append(' * ============================================================ */')
    lines.append('')
    lines.append('typedef struct {')
    lines.append('    const char *symbol;')
    lines.append('    uint32_t hex_id;')
    lines.append('    const char *desc_en;')
    lines.append('    const char *desc_zh;')
    lines.append('} error_code_t;')
    lines.append('')
    lines.append('/* ============================================================')
    lines.append(' * 错误代码数据库')
    lines.append(' * ============================================================ */')
    lines.append('')
    lines.append('static const error_code_t g_error_codes[] = {')
    
    for err in errors:
        symbol = err['symbol']
        hex_id = err['hex_id']
        desc_en = err['desc_en']
        desc_zh = err['desc_zh']
        lines.append(f'    {{ "{symbol}", {hex_id}, "{desc_en}", "{desc_zh}" }},')
    
    lines.append('};')
    lines.append('')
    lines.append('/* ============================================================')
    lines.append(' * 查找函数')
    lines.append(' * ============================================================ */')
    lines.append('')
    lines.append('const error_code_t* error_code_find(const char *symbol);')
    lines.append('const error_code_t* error_code_find_by_id(uint32_t hex_id);')
    lines.append('const char* error_code_get_desc(const char *symbol, const char *lang);')
    lines.append('')
    lines.append('#endif /* ERROR_CODES_H */')
    
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    
    print(f"Generated {output_path} from {json_path}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 gen_error_codes.py <input.json> <output.h>")
        sys.exit(1)
    
    json_path = sys.argv[1]
    output_path = sys.argv[2]
    
    if not os.path.exists(json_path):
        print(f"Error: {json_path} not found", file=sys.stderr)
        sys.exit(1)
    
    generate_header(json_path, output_path)