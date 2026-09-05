#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS Python 路径集中（先生 2026-09-05 裁决：路径以函数/常量集中，不逐文件硬编码）
用法: from paths import P_ETC, P_LOG, path_of, root 等
根 = 环境 LINGOS_ROOT（默认 /LINGOS——先生架构全并入 /LINGOS）
"""
import os

def root() -> str:
    """数据根：LINGOS_ROOT env 或 /LINGOS（先生架构）"""
    return os.environ.get("LINGOS_ROOT", "/LINGOS")

# ---- 标准子路径（先生 FHS 目标布局；兼容现有 system/config） ----
P_BIN   = root() + "/bin"
P_ETC   = root() + "/system/config"   # 配置（兼容现有；目标 /etc）
P_RUN   = root() + "/run"
P_LOG   = root() + "/log"
P_DATA  = root() + "/data"
P_STATE = root() + "/state"
P_MODELS= root() + "/models"
P_SKILLS= root() + "/skills"
P_SHARE = root() + "/share"
P_WEBUI = root() + "/share/webui"
P_REG   = root() + "/registry"
P_PLUGINS= root() + "/plugins"
P_EN    = root() + "/Ensystem"

def path_of(sub: str) -> str:
    """拼接根下相对路径（sub 以 / 开头）"""
    return root() + sub

def ensure(d: str) -> str:
    os.makedirs(d, exist_ok=True)
    return d
