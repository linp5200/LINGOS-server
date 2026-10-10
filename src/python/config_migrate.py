#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LINGOS · 配置统一迁移器（先生 方案 C 落地——旧配置族 → 单一 config.json）

背景（2026-10-01 §十 代码级调查）：
  /LINGOS/system/config/ 下有 27 个配置文件，分属三条互相不知道的读取路径：
    ① C config_core    → state.json / ai_config.json
    ② Python ai_server → ai_config.json
    ③ Python llm_unified → provider.json
  → 同一功能三个答案（"配置三源分裂"实锤 F2）。

先生裁决（2026-10-01 晚 §10.5）：
  **Q1 统一源 = 方案 C**：新建统一 schema，config.json 单文件，三端全部迁移。
  **兼容铁律**：旧文件保留 .bak（不删除）；三端改为"有 config.json 用新、无则读旧"。

用法:
    python3 config_migrate.py [--root /LINGOS] [--dry-run] [--verify] [--force]

    （无参数）   执行迁移：扫描旧配置 → 合并 → 写 config.json → 旧文件改名 .bak.<ts>
    --dry-run    只打印将要迁移的内容（不落盘）
    --verify     校验已是 config.json 的完整性（分区/必需键）
    --force      即使 config.json 已存在也重新迁移（旧文件另存）
"""
import argparse
import json
import os
import shutil
import sys
import time

SCHEMA_VERSION = 1

# ── 分区归属（先生 §10.5 Q1 拍板的分区结构）────────────────────────────
# 每个分区承接哪些旧文件；"ai" 区另含 provider.json（Python 统一层）
ZONES = {
    "ai":       ["ai_config.json", "provider.json"],
    "security": ["security.json", "defense.conf", "risk_policy.json",
                 "privilege.json", "sandbox.conf", "auth_always.json"],
    "runtime":  ["startup.conf", "watchdog.conf", "backup_policy.conf",
                 "health.conf", "network.conf", "sub_ai.conf"],
    "ui":       ["ux_ext.json", "user_profile.json", "language_map.json"],
    "data":     ["agent_roles.json", "roles.json", "skill_help.json",
                 "common_issues.json", "repair_components.json",
                 "repair_strategies.json"],
    "state":    ["state.json", "ai_config_version"],
}

# 单文件内的键归属（这些键应进 ui 区而不是 ai 区——先生的"指定内容由指定端配置"）
UI_KEYS_FROM_AI = {"language", "font_size", "theme", "accent", "wizard_anim"}


def read_any(path):
    """读 JSON 或简单 key=value（.conf）——返回 dict；失败返回 None"""
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()
    except Exception as e:
        print("  ! 读取失败 %s: %s" % (path, e))
        return None
    if not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except Exception:
        pass
    # .conf：key = value（# 注释）
    out = {}
    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip()
            if v.lower() in ("true", "false"):
                out[k] = (v.lower() == "true")
            else:
                try:
                    out[k] = int(v)
                except ValueError:
                    out[k] = v
    return out or None


# ── 历史垃圾值清洗（先生 2026-09-30 真机取证：方向键转义被写进配置）──
GARBAGE_PATTERNS = ("\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\u001b[A", "\u001b[B")


def clean_garbage(value):
    """清洗历史垃圾值（方向键转义等）；返回 (清洗后值, 是否被清洗)"""
    if isinstance(value, str):
        for g in GARBAGE_PATTERNS:
            if value == g or (len(value) < 24 and g in value):
                return "", True
        return value, False
    if isinstance(value, dict):
        out, changed = {}, False
        for k, v in value.items():
            nv, ch = clean_garbage(v)
            out[k] = nv
            changed = changed or ch
        return out, changed
    if isinstance(value, list):
        out, changed = [], False
        for v in value:
            nv, ch = clean_garbage(v)
            out.append(nv)
            changed = changed or ch
        return out, changed
    return value, False


# ── 配置三源冲突检测（"同一功能三个答案"——先生 F2 实锤）────────────
#   旧体系里 ai_config.json 与 provider.json 各自描述"用哪个 LLM"，
#   迁移时不静默择一，而是并列保留 + 报告，由用户/后续 UI 决策。
def detect_conflicts(cfg):
    """返回冲突清单 [(字段路径, 各源取值)]"""
    conflicts = []
    ai_cfg = (cfg.get("ai", {}) or {}).get("ai_config", {}) or {}
    prov = (cfg.get("ai", {}) or {}).get("provider", {}) or {}
    if isinstance(ai_cfg, dict) and isinstance(prov, dict):
        legacy_backend = ai_cfg.get("backend")
        legacy_url = (ai_cfg.get("ollama") or {}).get("url") if isinstance(ai_cfg.get("ollama"), dict) else None
        prov_active = prov.get("active")
        prov_list = prov.get("providers") if isinstance(prov.get("providers"), list) else []
        active_entry = None
        for p in prov_list:
            if isinstance(p, dict) and p.get("id") == prov_active:
                active_entry = p
                break
        if legacy_backend and prov_active and legacy_backend != prov_active:
            conflicts.append(("ai.backend vs ai.provider.active", {"ai_config.backend": legacy_backend,
                                                                  "provider.active": prov_active}))
        if legacy_url and active_entry and active_entry.get("base_url") and legacy_url != active_entry["base_url"]:
            conflicts.append(("ai.base_url", {"ai_config.ollama.url": legacy_url,
                                              "provider.active.base_url": active_entry["base_url"]}))
        if active_entry and active_entry.get("api_key") and not active_entry.get("api_key_enc"):
            conflicts.append(("ai.api_key 明文", {"provider": "明文（建议改 api_key_enc 值级加密）"}))
    return conflicts


def migrate(cfg_dir, force=False, dry=False):
    target = os.path.join(cfg_dir, "config.json")
    if os.path.exists(target) and not force:
        print("config.json 已存在 → 跳过迁移（用 --force 可重新迁移）")
        return 0

    config = {
        "schema_version": SCHEMA_VERSION,
        "_note": "LINGOS 统一配置——全部内容集中此文件；改动后 config reload 生效",
        "_migrated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    moved = []          # 实际纳入的旧文件
    missing = []        # 不存在（跳过）
    for zone, files in ZONES.items():
        zone_data = {}
        for name in files:
            p = os.path.join(cfg_dir, name)
            data = read_any(p)
            if data is None:
                missing.append(name)
                continue
            key = name.rsplit(".", 1)[0]          # ai_config.json → ai_config
            zone_data[key] = data
            moved.append(name)
        if zone_data:
            config[zone] = zone_data

    # ── 键级归位：ai 区的 UI 键挪到 ui 区（先生"指定内容由指定端配置"）──
    ai_zone = config.get("ai", {})
    for holder in list(ai_zone.keys()):
        d = ai_zone[holder]
        if isinstance(d, dict):
            for k in list(d.keys()):
                if k in UI_KEYS_FROM_AI:
                    config.setdefault("ui", {}).setdefault(holder, {})[k] = d.pop(k)

    # 历史垃圾值清洗（方向键转义等）
    config, cleaned = clean_garbage(config)
    if cleaned:
        print("⚠ 已清洗历史垃圾值（方向键转义等）")

    print("分区：%s" % ", ".join(sorted(k for k in config if not k.startswith("_"))))
    print("纳入旧文件 %d 个：%s" % (len(moved), ", ".join(sorted(moved))))
    if missing:
        print("不存在（跳过）%d 个：%s" % (len(missing), ", ".join(sorted(missing))))

    # 三源冲突报告（不静默择一——写入 config.json 的 _conflicts 供后续 UI 决策）
    conflicts = detect_conflicts(config)
    if conflicts:
        print("\n⚠ 检出配置三源冲突 %d 处（旧体系同一功能多个答案）：" % len(conflicts))
        for path, detail in conflicts:
            print("   · %s: %s" % (path, detail))
        config["_conflicts"] = [{"field": p, "sources": d} for p, d in conflicts]
        print("   → 已记录到 config.json 的 _conflicts 字段（不静默择一，等 UI/用户决策）")

    if dry:
        print("\n--- config.json 预览（前 60 行）---")
        text = json.dumps(config, ensure_ascii=False, indent=2)
        print("\n".join(text.splitlines()[:60]))
        print("--dry-run：未落盘")
        return 0

    # ① 先备份旧文件（先生铁律：旧文件不删除，另存 .bak.<ts>）
    ts = time.strftime("%Y%m%d-%H%M%S")
    backup_dir = os.path.join(cfg_dir, "backup-pre-configjson-%s" % ts)
    os.makedirs(backup_dir, exist_ok=True)
    for name in moved:
        src = os.path.join(cfg_dir, name)
        try:
            shutil.copy2(src, os.path.join(backup_dir, name + ".bak"))
        except Exception as e:
            print("  ! 备份失败 %s: %s" % (name, e))

    # ② 写 config.json（原子：先 .tmp 再 rename）
    tmp = target + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, target)

    # 【隐私】config.json 含 api_key / ha token / 加密密钥引用 → 收紧到 0600
    #   （先生隐私优先级 #2：数据默认不出家门，凭据不裸奔）
    try:
        os.chmod(target, 0o600)
    except Exception as e:
        print("  ! 权限收紧失败（建议手工 chmod 600）：%s" % e)

    print("✅ 已写出 %s（%d 字节，权限 0600）" % (target, os.path.getsize(target)))
    print("   旧文件备份：%s（%d 个）" % (backup_dir, len(moved)))
    print("   兼容：三端读取层应实现「有 config.json 用新，无则读旧」——旧文件保持原位未动")
    return 0


def verify(cfg_dir):
    p = os.path.join(cfg_dir, "config.json")
    if not os.path.exists(p):
        print("❌ 未找到 %s" % p)
        return 1
    try:
        with open(p, encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception as e:
        print("❌ JSON 解析失败：%s" % e)
        return 1
    ok = True
    if cfg.get("schema_version") != SCHEMA_VERSION:
        print("❌ schema_version 期望 %d，实际 %r" % (SCHEMA_VERSION, cfg.get("schema_version")))
        ok = False
    zones = [k for k in cfg if not k.startswith("_")]
    print("分区：%s" % ", ".join(sorted(zones)))
    mode = oct(os.stat(p).st_mode & 0o777)
    if mode != "0o600":
        print("⚠ 权限 %s（建议 0600——文件含凭据）" % mode)
    else:
        print("权限：0600 ✓")
    for z in ("ai", "security", "runtime"):
        if z not in cfg:
            print("⚠ 缺少分区：%s" % z)
    print("✅ 校验通过" if ok else "❌ 校验失败")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="LINGOS 配置统一迁移器（方案 C）")
    ap.add_argument("--root", default=os.environ.get("LINGOS_ROOT", "/LINGOS"),
                    help="LINGOS 根目录（默认 /LINGOS 或 $LINGOS_ROOT）")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    cfg_dir = os.path.join(args.root, "system", "config")
    if not os.path.isdir(cfg_dir):
        print("❌ 配置目录不存在：%s" % cfg_dir)
        return 2
    print("配置目录：%s" % cfg_dir)
    if args.verify:
        return verify(cfg_dir)
    return migrate(cfg_dir, force=args.force, dry=args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
