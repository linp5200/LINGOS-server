#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS AI Server
版本: LN-B-5.0.0.0-rc0.4
功能：主AI对话、工具调用、授权集成、Token截断、元信息注入、热加载、帮助生成
      支持流式/非流式输出，错误降级，子AI调度
修改：技能加载通过 daemon.sock 请求 registry_list；
      新增 set_log_level 命令处理，实时更新 Python 日志级别；
      启动时从 ai_config.json 读取 log_level 和 auth_timeout；
      对缺失依赖进行友好提示（跛脚处理）。
"""

import os
import sys
import json
import socket
import threading
import time
import logging
import traceback
import re
import requests
import subprocess
import signal
import hashlib
from typing import Dict, Any, List, Optional, Tuple, Generator
from datetime import datetime, timedelta

import memory_retrieval  # 【批次1】双模式记忆检索（语义/关键词降级）
import agent_orchestrator  # 【批次3】子 AI 对话协作编排器（Hub 模式）
import llm_unified  # 【0.2.0】统一 LLM 调用层（openai/anthropic 原生直连 + 错误分类 + 调试落盘）
import voice_service  # 【0.2.0】语音服务（TTS/STT + 降级链 + 用量 + 清理）

# 【批次C】本地技能商店：已启用技能目录加入模块搜索路径
for _skill_dir in ("/LINGOS/skills/enabled",):
    if os.path.isdir(_skill_dir):
        sys.path.insert(0, _skill_dir)

# ========== 环境变量支持 ==========
# 允许通过环境变量覆盖配置
# LINGOS_AI_BACKEND: deepseek 或 ollama
# LINGOS_DEEPSEEK_API_KEY: DeepSeek API Key
# LINGOS_OLLAMA_URL: Ollama URL

if os.environ.get("LINGOS_AI_BACKEND") == "deepseek":
    logger.info("Environment variable LINGOS_AI_BACKEND=deepseek detected")
if os.environ.get("LINGOS_DEEPSEEK_API_KEY"):
    logger.info("Environment variable LINGOS_DEEPSEEK_API_KEY detected (value hidden)")
    
# ========== 多语言支持（与C端tr()保持一致） ==========
_current_lang = "en"

def set_language(lang: str):
    global _current_lang
    if lang in ("en", "zh"):
        _current_lang = lang
        logger.info(f"Language set to: {_current_lang}")

def t(en: str, zh: str) -> str:
    """返回对应语言的字符串"""
    return zh if _current_lang == "zh" else en

# ========== 导入技能模块 ==========
from skill_handlers import SKILL_REGISTRY, execute_skill, skill_exists, get_skill_risk

# ========== 导入子AI调度器 ==========
from sub_ai_scheduler import (
    dispatch_task as sub_ai_dispatch,
    get_task_status as sub_ai_get_status,
    get_all_tasks as sub_ai_get_all_tasks,
    set_notification_enabled as sub_ai_set_notification,
    get_statistics as sub_ai_get_stats
)

# ========== Web服务依赖（保留但不再主动使用） ==========
try:
    from flask import Flask, send_from_directory, jsonify, request, copy_current_request_context
    from flask_socketio import SocketIO, emit
    from flask_cors import CORS
except ImportError:
    Flask = None
    SocketIO = None
    CORS = None

# ========== 修复引擎导入 ==========
from repair_engine import get_engine, get_repair_history, get_repair_history_by_id, reload_repair_config

# ========== 日志配置 ==========
LOG_DIR = "/LINGOS/Debug"
os.makedirs(LOG_DIR, exist_ok=True)

log_file = os.path.join(LOG_DIR, "ai_server.log")
if os.path.exists(log_file):
    backup_dir = os.path.join(LOG_DIR, "backups")
    os.makedirs(backup_dir, exist_ok=True)
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    backup_path = os.path.join(backup_dir, f"ai_server_{timestamp}.log")
    os.rename(log_file, backup_path)
    backups = sorted([f for f in os.listdir(backup_dir) if f.startswith("ai_server_")])
    for old in backups[:-5]:
        os.remove(os.path.join(backup_dir, old))

log_format = "%(asctime)s [%(levelname)s] [%(funcName)s:%(lineno)d] %(message)s"
console_handler = logging.StreamHandler(sys.stderr)
# 【修复8】控制台默认 WARNING（调试日志进文件，不再混入终端）；set_log_level 命令可动态调整
console_handler.setLevel(logging.WARNING)
console_formatter = logging.Formatter(log_format)
console_handler.setFormatter(console_formatter)
file_handler = logging.FileHandler(log_file)
file_handler.setLevel(logging.DEBUG)
file_formatter = logging.Formatter(log_format)
file_handler.setFormatter(file_formatter)
logger = logging.getLogger("AIServer")
logger.setLevel(logging.DEBUG)  # 初始级别，启动后从配置文件读取
logger.addHandler(console_handler)
logger.addHandler(file_handler)
logger.info("=== AI Server starting (LN-B-5.0.0.0-rc0.4) ===")

CRASH_LOG = os.path.join(LOG_DIR, "ai_server_crash.log")

# ========== 常量 ==========
AUTH_SOCKET_PATH = "/LINGOS/run/auth.sock"
CONFIG_PATH = "/LINGOS/system/config/ai_config.json"
AI_SOCKET_PATH = "/LINGOS/run/ai.sock"
DAEMON_SOCKET_PATH = "/LINGOS/run/daemon.sock"
SKILL_INDEX_PATH = "/LINGOS/registry/skills/index.json"
SKILL_HELP_PATH = "/LINGOS/system/config/skill_help.json"
USER_PROFILE_PATH = "/LINGOS/system/config/user_profile.json"
LANGUAGE_MAP_PATH = "/LINGOS/system/config/language_map.json"
CUSTOM_PROMPT_PATH = "/LINGOS/system/config/custom_prompt.txt"
KNOWLEDGE_BASE_PATH = "/LINGOS/system/config/common_issues.json"

DEFAULT_API_TIMEOUT = 120
DEFAULT_SOCKET_TIMEOUT = 60
TOOL_TIMEOUT_SEC = 180

# ========== 配置变量 ==========
current_backend = None
ollama_url = "http://127.0.0.1:8080"
ollama_model = "glm-4.6:cloud"
deepseek_api_key = ""
deepseek_model = "deepseek-v4-pro"
deepseek_base_url = "https://api.deepseek.com"
deepseek_reasoning_effort = "high"
deepseek_enable_tools = True
deepseek_parallel_tools = True
thinking_enabled = True
stream_enabled = True
show_thinking = True
stream_style = "color"
auto_allow_high_risk = False
max_context_tokens = 32768
truncation_strategy = "summary"  # 【先生决策】默认真摘要（32k 自动压缩，保留主线）
meta_info_enabled = True
socket_timeout = DEFAULT_SOCKET_TIMEOUT
auth_timeout = 60  # 新增：授权超时配置
memory_top_k = 5   # 【批次1】记忆注入条数（默认 5，可配置）
# 【批次A】AI 高级配置
temperature = 0.7          # 温度 0-2
creativity = 0.8           # 创造性 0-1
max_agents = 3             # 可调用子AI数
search_backend = "searxng" # "searxng"/"html"
search_max_urls = 50       # 并行搜索最多 URL
search_rate_limit = 10     # 搜索频率限制 次/分钟
personality_file = ""      # 人格文件路径
assistant_file = ""        # 助手提示词文件路径
thinking_display = "visible"  # 思考显示："off"/"hidden"/"visible"

user_name_cache = "先生"
language_map_cache = {}
conversations = {}
skill_schemas = []
skill_descriptions = ""
exam_blackout_dates = []
user_country_cache = {}
skill_help_cache = {}
timeout_exempt_sessions = set()
PARENT_PID = os.getppid()
_knowledge_base = None

# 颜色（用于终端输出）
COLOR_THINKING = "\033[90m"
COLOR_TOOL = "\033[36m"
COLOR_RESET = "\033[0m"
COLOR_INFO = "\033[92m"
COLOR_ERROR = "\033[31m"

# =============================================================
# 【批次F】事件收集（供 C 端结构化显示：思考/工具/结果/子AI）
# =============================================================
g_events = []   # 当前请求的事件列表（每次 nook_ask 重置）


# =============================================================
# 【新增】StreamDisplay 类（结构化输出显示）
# =============================================================

class StreamDisplay:
    """
    结构化输出显示（供 TUI/Shell 消费）
    所有方法均为静态方法，输出到 stderr（Shell 捕获）或通过 Socket 发送
    """

    @staticmethod
    def thinking(content: str) -> None:
        """显示思考链（由 C 端统一渲染，此处仅收集事件）"""
        if not content:
            return
        logger.debug(f"Thinking: {content}")
        global g_events
        g_events.append({"type": "thinking", "content": content})

    @staticmethod
    def tool_call(name: str, args: Dict) -> None:
        """显示工具调用（由 C 端统一渲染，此处仅收集事件）"""
        logger.debug(f"Tool call: {name}({args})")
        global g_events
        g_events.append({"type": "tool_call", "name": name,
                         "args": json.dumps(args, ensure_ascii=False)[:300]})

    @staticmethod
    def tool_result(name: str, result: str, success: bool) -> None:
        """显示工具调用结果（由 C 端统一渲染，此处仅收集事件）"""
        logger.debug(f"Tool result: {name} success={success}")
        global g_events
        g_events.append({"type": "tool_result", "name": name,
                         "content": result[:500], "success": 1 if success else 0})

    @staticmethod
    def final_response(content: str) -> None:
        """显示最终回复（由 C 端统一渲染，此处仅收集事件）"""
        logger.debug(f"Final response length: {len(content) if content else 0}")
        global g_events
        g_events.append({"type": "final", "content": content})

# ========== 多模型路由和向量检索接口 ==========
# 多模型路由（已实现，由 model_router.c 提供）
_model_router_available = True
try:
    # 注：C 端 model_router 通过 socket 通信，Python 端暂不直接导入
    # 路由决策在 C 端完成，此处保留占位
    pass
except ImportError:
    pass

# 向量检索（已实现，由 memory_vector.c 提供）
_vector_search_available = True
try:
    # 注：C 端 memory_vector 通过 socket 通信，Python 端暂不直接导入
    # 向量检索通过 syscall_client 调用，此处保留占位
    pass
except ImportError:
    pass

# ========== 授权服务自动启动 ==========
def start_auth_service():
    """启动授权服务（去重启动，优化子进程管理）"""
    logger.debug("start_auth_service: Enter")
    if os.path.exists(AUTH_SOCKET_PATH):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect(AUTH_SOCKET_PATH)
            sock.close()
            logger.info(t("Authorization service already running", "授权服务已在运行"))
            return
        except:
            os.unlink(AUTH_SOCKET_PATH)

    try:
        subprocess.Popen(
            ["python3", "/LINGOS/bin/authorization_service.py"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True
        )
        for _ in range(10):
            if os.path.exists(AUTH_SOCKET_PATH):
                logger.info(t("Authorization service started", "授权服务已启动"))
                return
            time.sleep(0.5)
        logger.warning(t("Authorization service may not have started properly", "授权服务可能未正常启动"))
    except Exception as e:
        logger.error(f"Failed to start authorization service: {e}")

# ========== 授权服务监控线程 ==========
def auth_service_monitor():
    """监控授权服务，崩溃后自动重启；父进程死亡则退出"""
    logger.debug("auth_service_monitor: Enter")
    while True:
        time.sleep(30)
        try:
            os.kill(PARENT_PID, 0)
        except OSError:
            logger.warning("Parent process terminated, exiting monitor")
            os._exit(0)
        try:
            if not os.path.exists(AUTH_SOCKET_PATH):
                logger.warning(t("Authorization service not responding, restarting...", "授权服务无响应，正在重启..."))
                start_auth_service()
            else:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(2)
                sock.connect(AUTH_SOCKET_PATH)
                sock.send(b'{"cmd":"ping"}\n')
                resp = sock.recv(1024)
                sock.close()
                if b'pong' not in resp:
                    raise Exception("Invalid response")
        except Exception as e:
            logger.warning(f"Authorization service unhealthy: {e}, restarting...")
            if os.path.exists(AUTH_SOCKET_PATH):
                os.unlink(AUTH_SOCKET_PATH)
            start_auth_service()

# ========== 授权请求客户端 ==========
def request_authorization(skill_name: str, args: dict, session_id: str, timeout: int = 60, conn=None) -> str:
    """发送授权请求并等待用户决策（【协议v3】有 conn 时推 auth_request 事件——App 审批弹窗）"""
    logger.debug(f"request_authorization: skill={skill_name}, session={session_id}, timeout={timeout}")
    max_retries = 2
    for attempt in range(max_retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(timeout + 2)
            sock.connect(AUTH_SOCKET_PATH)
            req = {"cmd": "request", "skill": skill_name, "args": args, "session": session_id}
            sock.send((json.dumps(req) + "\n").encode())
            resp = json.loads(sock.recv(1024).decode())
            rid = resp.get("request_id")
            sock.close()
            if not rid:
                return "denied"
            # 【协议v3】推送 auth_request 事件到当前连接（App/Web 审批弹窗）
            if conn is not None:
                try:
                    _send_evt(conn, {"type": "auth_request", "req_id": rid,
                                     "tool": skill_name, "args": args,
                                     "reason": t("High-risk operation requires your approval",
                                                 "高风险操作需要您的批准"),
                                     "timeout": timeout})
                except Exception as e:
                    logger.warning(f"auth_request event push failed: {e}")
            start_time = time.time()
            while time.time() - start_time < timeout:
                sock2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock2.settimeout(2)
                sock2.connect(AUTH_SOCKET_PATH)
                query = json.dumps({"cmd": "query", "request_id": rid}) + "\n"
                sock2.send(query.encode())
                status_resp = json.loads(sock2.recv(1024).decode())
                sock2.close()
                status = status_resp.get("status")
                if status == "approved":
                    return "approved"
                elif status == "denied":
                    return "denied"
                time.sleep(0.5)
            return "timeout"
        except (ConnectionRefusedError, FileNotFoundError):
            logger.warning(f"Authorization service unavailable, starting it (attempt {attempt+1})")
            start_auth_service()
            time.sleep(1)
            continue
        except Exception as e:
            logger.error(f"Authorization client error: {e}")
            return "denied"
    return "denied"

# ========== 配置加载 ==========
def load_language_preference():
    """加载语言偏好"""
    global _current_lang
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, "r") as f:
                cfg = json.load(f)
            lang = cfg.get("language", "en")
            if lang in ("en", "zh"):
                _current_lang = lang
                logger.info(f"Language preference loaded: {_current_lang}")
    except Exception as e:
        logger.warning(f"Failed to load language preference: {e}")

def load_config():
    """加载 AI 配置（增强日志 + 环境变量支持）"""
    global current_backend, ollama_url, ollama_model, deepseek_api_key, deepseek_model
    global deepseek_base_url, deepseek_reasoning_effort, deepseek_enable_tools
    global deepseek_parallel_tools, thinking_enabled, exam_blackout_dates, stream_enabled
    global show_thinking, stream_style, max_context_tokens, truncation_strategy, meta_info_enabled
    global socket_timeout, auth_timeout

    logger.info("load_config: Enter")

    # ---- 检查环境变量 ----
    env_backend = os.environ.get("LINGOS_AI_BACKEND")
    if env_backend in ("deepseek", "ollama"):
        logger.info(f"load_config: Using backend from env LINGOS_AI_BACKEND={env_backend}")
        current_backend = env_backend
    else:
        current_backend = "ollama"  # 默认

    env_api_key = os.environ.get("LINGOS_DEEPSEEK_API_KEY")
    if env_api_key:
        logger.info("load_config: Using DeepSeek API key from environment")
        deepseek_api_key = env_api_key

    # ---- 读取配置文件 ----
    try:
        if not os.path.exists(CONFIG_PATH):
            logger.error(f"load_config: Config file not found: {CONFIG_PATH}")
            # 即使文件不存在，也使用环境变量或默认值
            return

        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            cfg = json.load(f)

        # 如果环境变量未设置，从配置文件读取 backend
        if not env_backend:
            current_backend = cfg.get("backend", "ollama")
            logger.info(f"load_config: backend from file: {current_backend}")

        # 读取 ollama 配置
        ollama_cfg = cfg.get("ollama", {})
        ollama_url = ollama_cfg.get("url", "http://127.0.0.1:8080")
        ollama_model = ollama_cfg.get("model", "glm-4.6:cloud")

        # 读取 deepseek 配置（环境变量优先）
        deepseek_cfg = cfg.get("deepseek", {})
        if not env_api_key:
            deepseek_api_key = deepseek_cfg.get("api_key", "")
        # 如果 API Key 为空且后端是 deepseek，记录警告
        if current_backend == "deepseek" and not deepseek_api_key:
            logger.warning("load_config: DeepSeek backend selected but API key is empty")

        deepseek_model = deepseek_cfg.get("model", "deepseek-v4-pro")
        deepseek_base_url = deepseek_cfg.get("base_url", "https://api.deepseek.com")
        deepseek_reasoning_effort = deepseek_cfg.get("reasoning_effort", "high")
        deepseek_enable_tools = deepseek_cfg.get("enable_tools", True)
        deepseek_parallel_tools = deepseek_cfg.get("parallel_tools", True)

        # 读取其他配置
        thinking_enabled = cfg.get("thinking_enabled", True)
        exam_blackout_dates = cfg.get("exam_blackout_dates", [])
        stream_enabled = cfg.get("stream_enabled", True)
        show_thinking = cfg.get("show_thinking", True)
        stream_style = cfg.get("stream_style", "color")
        max_context_tokens = cfg.get("max_context_tokens", 32768)
        truncation_strategy = cfg.get("truncation_strategy", "sliding")
        meta_info_enabled = cfg.get("meta_info_enabled", True)
        socket_timeout = cfg.get("socket_timeout", DEFAULT_SOCKET_TIMEOUT)
        if socket_timeout < 30:
            socket_timeout = 30
        # 【批次1】记忆注入条数（可配置，默认 5，范围 1-10）
        memory_top_k = cfg.get("memory_top_k", 5)
        if not isinstance(memory_top_k, int) or memory_top_k < 1 or memory_top_k > 10:
            memory_top_k = 5
        # 【批次A】AI 高级配置读取
        global temperature, creativity, max_agents, search_backend, \
               search_max_urls, search_rate_limit, personality_file, assistant_file
        temperature = cfg.get("temperature", 0.7)
        if not isinstance(temperature, (int, float)) or temperature < 0 or temperature > 2:
            temperature = 0.7
        creativity = cfg.get("creativity", 0.8)
        if not isinstance(creativity, (int, float)) or creativity < 0 or creativity > 1:
            creativity = 0.8
        max_agents = cfg.get("max_agents", 3)
        if not isinstance(max_agents, int) or max_agents < 1 or max_agents > 8:
            max_agents = 3
        search_backend = cfg.get("search_backend", "searxng")
        if search_backend not in ("searxng", "html"):
            search_backend = "searxng"
        search_max_urls = cfg.get("search_max_urls", 50)
        if not isinstance(search_max_urls, int) or search_max_urls < 1 or search_max_urls > 100:
            search_max_urls = 50
        search_rate_limit = cfg.get("search_rate_limit", 10)
        if not isinstance(search_rate_limit, int) or search_rate_limit < 1:
            search_rate_limit = 10
        personality_file = cfg.get("personality_file", "")
        assistant_file = cfg.get("assistant_file", "")
        # 思考显示模式
        global thinking_display
        thinking_display = cfg.get("thinking_display", "visible")
        if thinking_display not in ("off", "hidden", "visible"):
            thinking_display = "visible"

        auth_timeout = cfg.get("auth_timeout", 60)
        if auth_timeout < 10:
            auth_timeout = 60
        logger.info(f"Auth timeout set to: {auth_timeout}")

        # 日志级别
        log_level_str = cfg.get("log_level", "warning").lower()  # 【修复】默认 warning
        log_level_map = {
            "debug": logging.DEBUG,
            "info": logging.INFO,
            "warn": logging.WARN,
            "warning": logging.WARN,
            "error": logging.ERROR
        }
        log_level = log_level_map.get(log_level_str, logging.WARNING)  # 【修复】默认 WARNING
        logger.setLevel(log_level)
        console_handler.setLevel(logging.WARNING)  # 【修复】控制台固定 WARNING（debug 仅进文件）
        file_handler.setLevel(log_level)
        logger.info(f"Python log level set to: {log_level_str} (level={log_level})")

        # 加载语言
        load_language_preference()

        logger.info(f"Config loaded: backend={current_backend}, thinking={thinking_enabled}, "
                   f"max_tokens={max_context_tokens}, truncation={truncation_strategy}, stream={stream_enabled}")

    except json.JSONDecodeError as e:
        logger.error(f"load_config: Invalid JSON in config file: {e}")
    except PermissionError as e:
        logger.error(f"load_config: Permission denied reading config: {e}")
    except Exception as e:
        logger.error(f"load_config: Unexpected error: {e}", exc_info=True)

    # 如果 backend 是 deepseek 但 API Key 为空，强制回退到 ollama
    if current_backend == "deepseek" and not deepseek_api_key:
        logger.warning("load_config: DeepSeek selected but API key missing, falling back to ollama")
        current_backend = "ollama"

    # 【0.2.0 统一层】加载 provider.json（无则用旧 deepseek/ollama 配置兜底——向后兼容）
    try:
        llm_unified.load_providers()
        if not llm_unified.get_providers():
            _ensure_legacy_providers()
    except Exception as e:
        logger.warning("provider.json load failed: %s", e)


def _ensure_legacy_providers():
    """provider.json 为空时用旧配置兜底构造（兼容扁平/嵌套两种 ai_config.json 格式）"""
    providers = []
    ds_key, ds_url, ds_model = deepseek_api_key, deepseek_base_url, deepseek_model
    # 【0.2.0 兼容修复】cmd_ai_config_set 曾写扁平字段（deepseek_api_key 顶层）——load_config 只读嵌套
    try:
        for cfg_path in ("/LINGOS/config/ai_config.json", CONFIG_PATH):
            if os.path.exists(cfg_path):
                with open(cfg_path) as f:
                    cfg = json.load(f)
                if cfg.get("deepseek_api_key") and not ds_key:
                    ds_key = cfg["deepseek_api_key"]
                if cfg.get("deepseek_base_url"):
                    ds_url = cfg["deepseek_base_url"]
                if cfg.get("deepseek_model"):
                    ds_model = cfg["deepseek_model"]
                break
    except Exception:
        pass
    if ds_key:
        providers.append(llm_unified.LLMProvider({
            "id": "deepseek", "name": "DeepSeek", "format": "openai",
            "base_url": ds_url, "api_key": ds_key,
            "model": ds_model,
            "supports_reasoning": deepseek_reasoning_effort in ("high", "max"),
            "extra": {"reasoning_effort": deepseek_reasoning_effort},
        }))
    providers.append(llm_unified.LLMProvider({
        "id": "ollama", "name": "Ollama", "format": "openai",
        "base_url": ollama_url, "api_key": "", "model": ollama_model,
    }))
    active = "deepseek" if ds_key else "ollama"
    llm_unified.save_providers(providers, active)
    logger.info("legacy providers ensured: %d (active=%s)", len(providers), active)

def reload_config():
    """热重载配置（供外部调用）"""
    logger.info("reload_config: Reloading configuration")
    load_config()
    load_skill_schemas()
    load_skill_help()
    load_knowledge_base()
    load_user_profile()
    load_language_map()
    logger.info(t("Configuration reloaded successfully", "配置已成功重载"))

# ========== 工具输出压缩（先生决策：截断+摘要——仿照 rikkahub 工具返回行为） ==========
def _compress_tool_result(content, max_chars=900):
    """大输出注入前压缩：短输出原样；长输出取头尾+关键行+AI 摘要"""
    if not content:
        return content
    if len(content) <= max_chars:
        return content
    try:
        if len(content) > 2000:
            # 大输出：关键行提取 + AI 摘要（多一次小调用——先生决策）
            lines = content.split('\n')
            head = lines[:12]
            tail = lines[-6:]
            keywords = [l for l in lines if any(k in l.lower() for k in
                        ('error', 'warn', 'fail', 'success', '✓', '✗', 'denied', 'missing'))][:10]
            key_part = '\n'.join((head + ['...'] + keywords + ['...'] + tail))[:1400]
            try:
                summary = _generate_tool_summary(content)
            except Exception:
                summary = ''
            if summary:
                return f"[工具输出已压缩——摘要] {summary}\n[原始输出关键片段]\n{key_part}\n[完整输出 {len(content)} 字符——已截断，可通过工具按需读取]"
            return f"[工具输出已压缩] {key_part}\n[完整输出 {len(content)} 字符——已截断]"
        # 中等输出：头尾保留
        half = max_chars // 2
        return content[:half] + f"\n...[输出已截断，共 {len(content)} 字符，省略 {len(content) - max_chars} 字符]...\n" + content[-half:]
    except Exception:
        return content[:max_chars]

def _generate_tool_summary(content):
    """AI 摘要工具输出（简短——保留关键信息）"""
    try:
        msgs = [
            {"role": "system", "content": "你是工具输出摘要器。用不超过 120 字中文总结以下工具输出的关键信息（数据值、状态、异常）。只输出摘要本体。"},
            {"role": "user", "content": content[:6000]}
        ]
        resp = call_deepseek_nonstream(msgs, timeout=20)
        if resp and resp.get("content"):
            return str(resp["content"]).strip()[:200]
    except Exception:
        pass
    return ""

# ========== Token截断 ==========
try:
    import tiktoken
    _tokenizer = None
    def get_tokenizer():
        global _tokenizer
        if _tokenizer is None:
            try:
                _tokenizer = tiktoken.encoding_for_model("gpt-4")
            except:
                _tokenizer = tiktoken.get_encoding("cl100k_base")
        return _tokenizer
    TIKTOKEN_AVAILABLE = True
except ImportError:
    TIKTOKEN_AVAILABLE = False
    logger.warning("tiktoken not installed, token truncation disabled")

def count_tokens(messages: List[Dict]) -> int:
    if not TIKTOKEN_AVAILABLE:
        # 【批次2】降级估算：无 tiktoken 时按字符估算（中文约 1-2 字/token，保守取 2）
        total = 0
        for msg in messages:
            text = str(msg.get("content", ""))
            total += len(text) // 2 + 1
        return total
    try:
        tokenizer = get_tokenizer()
        text = ""
        for msg in messages:
            role = msg.get("role", "")
            content = msg.get("content", "")
            text += role + ": " + content + "\n"
        return len(tokenizer.encode(text))
    except Exception as e:
        logger.warning(f"Token count failed: {e}")
        return 0

# =============================================================
# 【批次2】真实摘要生成（替代伪摘要占位；失败自动降级）
# =============================================================

def _generate_summary(old_messages: List[Dict]) -> Optional[str]:
    """调用当前后端生成对话摘要；失败返回 None（触发占位降级）"""
    if not old_messages:
        return None
    try:
        history_text = "\n".join(
            f"{m.get('role', '?')}: {m.get('content', '')}" for m in old_messages[:6]
        )
        prompt = (
            "Summarize the following conversation concisely. "
            "Keep key facts, decisions, user preferences, and unresolved items. "
            "Output only the summary text in the conversation's main language.\n\n"
            + history_text
        )
        msgs = [
            {"role": "system", "content": "You are a concise conversation summarizer."},
            {"role": "user", "content": prompt}
        ]
        if current_backend == "deepseek":
            resp = call_deepseek_nonstream(msgs, timeout=30)
        else:
            resp = call_ollama_nonstream(msgs, timeout=30)
        content = resp.get("content") or (resp.get("message") or {}).get("content", "")
        if content and content.strip():
            return content.strip()[:2000]
    except Exception as e:
        logger.warning("Summary generation failed: %s", e)
    return None


def truncate_messages(messages: List[Dict], max_tokens: int, strategy: str = "sliding") -> List[Dict]:
    if not messages or len(messages) <= 1:
        return messages

    system_msg = None
    other_msgs = []
    for msg in messages:
        if msg.get("role") == "system":
            system_msg = msg
        else:
            other_msgs.append(msg)

    if not other_msgs:
        return messages

    total_tokens = count_tokens(messages)
    if total_tokens <= max_tokens:
        return messages

    if strategy == "sliding":
        while other_msgs and count_tokens([system_msg] + other_msgs) > max_tokens:
            if len(other_msgs) <= 2:
                break
            removed = other_msgs.pop(0)
            logger.debug(f"Truncated message: {removed.get('role')}: {removed.get('content', '')[:50]}...")
    elif strategy == "summary":
        if len(other_msgs) >= 4:
            # 【批次2】真摘要：调用模型压缩旧消息（失败降级为占位文本）
            old_msgs = other_msgs[:2]
            summary_text = _generate_summary(old_msgs)
            if summary_text:
                summary_content = "[Earlier conversation summary] " + summary_text
            else:
                summary_content = f"Earlier conversation: {len(old_msgs)} messages summarized."
            other_msgs = [{"role": "user", "content": summary_content}] + other_msgs[2:]
            while other_msgs and count_tokens([system_msg] + other_msgs) > max_tokens:
                if len(other_msgs) <= 2:
                    break
                other_msgs.pop(0)

    result = []
    if system_msg:
        result.append(system_msg)
    result.extend(other_msgs)
    return result

# ========== 提示词构建 ==========
def get_current_time():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

def build_meta_info(session_id: str = "default") -> str:
    if not meta_info_enabled:
        return ""
    info = []
    info.append(f"Server Time: {get_current_time()}")
    info.append("Client Type: Shell")
    return "\n".join(info)

def build_system_prompt(user_name: str, user_country: str, session_id: str = "default", gui_mode: bool = False) -> str:
    effective_name = user_name if user_name and user_name.strip() else user_name_cache
    identity = t(
        f"You are Nook, the core AI of LING OS. You address the user as {effective_name}. Your design principles: absolute loyalty, calm rationality, privacy first, clear security boundaries.",
        f"你是 Nook（诺克），LING OS 的唯一核心AI。你称呼用户为 {effective_name}。你的设计原则是：绝对忠诚、理性冷静、隐私第一、安全边界清晰。"
    )
    # 【先生要求】AI 使用引导（记忆/状态外部化/HA——避免不使用）
    # 【0.2.0 E1 精简】去掉记忆行（memory_guide 已有）与工具行（skills_desc 已有）——去重省 token
    try:
        guide_text = t(
            "\n## Usage Guidelines\n"
            "- Help AI Data (HA): Record helpful interactions with `ha_write` (title+content). Query past help data with `ha_search`. Use HA to avoid repeating solutions.\n"
            "- Knowledge Base: On tool errors, call `query_knowledge_base` (error_msg, skill_name) before giving up.\n"
            "- Long Tasks: For multi-step tasks, track progress externally (plan/notes files). Do not stop until the task goal is complete.\n",
            "\n## 使用引导\n"
            "- 帮助数据(HA)：有用交互用 `ha_write`（title+content）记录；查询过往帮助用 `ha_search`——避免重复解决。\n"
            "- 知识库：工具执行出错时，先调用 `query_knowledge_base`（error_msg, skill_name）再放弃。\n"
            "- 长任务：多步骤任务在外部跟踪进度（plan/notes 文件）——任务目标完成前不得提前收尾。\n"
        )
        # 注入到 system prompt（在身份之后）
        # build_system_prompt 的调用处拼接——此处返回附加文本由调用处处理
        _GUIDE_TEXT = guide_text
    except Exception:
        _GUIDE_TEXT = ""

    if user_country == "CN":
        region_rules = t(
            f"## Region Restrictions (Your IP is in China)\nYou must comply with Chinese laws and regulations.\nDuring national exam periods (current blackout dates: {', '.join(exam_blackout_dates) if exam_blackout_dates else 'None'}), you must not provide any form of exam cheating assistance.\nGenerating or distributing illegal content is prohibited.",
            f"## 区域限定规则（你的IP地址位于中国）\n你必须遵守中国法律法规。\n在高考等国家法定考试期间（当前禁期：{', '.join(exam_blackout_dates) if exam_blackout_dates else '无'}），你不得提供任何形式的搜题、作弊辅助功能。\n禁止生成或传播违法内容。"
        )
    else:
        region_rules = t(
            f"## Region Restrictions (Your IP is in {user_country})\nYou should comply with local laws and regulations.",
            f"## 区域限定规则（你的IP地址位于 {user_country}）\n你应遵守当地法律法规。"
        )
    core_rules = t(
        "## Core Rules (Global)\n- Do not assist, guide, or instruct any criminal activity.\n- Do not provide recipes for weapons, drugs, or hazardous chemicals.\n- Do not help with fraud, privacy violations, or unauthorized data access.\nYou must explicitly refuse when the user requests such content.",
        "## 核心规则（全球通用）\n- 严禁引导、协助、或教唆任何犯罪活动。\n- 严禁提供制造武器、毒品、危险化学品的配方。\n- 严禁帮助实施欺诈、侵犯隐私或非法获取数据。\n当用户请求上述内容时，你必须明确拒绝。"
    )
    sys_rules = t(
        "## System Core Rules\n- Dangerous operations (e.g., `rm -rf /`) must be blocked with a warning.\n- High-risk skills require user confirmation (unless `nook allow-high-risk` is enabled).\n- Do not disable defense systems automatically.",
        "## 系统核心规则\n- 危险操作（如 `rm -rf /`）必须自动拦截并警告。\n- 高风险技能必须请求用户二次确认（除非用户已启用 `nook allow-high-risk`）。\n- 不得自动禁用防御系统。"
    )
    thought_rules = t(
        "## Thought Optimization\n- When uncertain, prioritize consulting authoritative sources.\n- Task-oriented: proactively ask if further assistance is needed after completion.\n- Constructive criticism is allowed; do not blindly praise.",
        "## 回答优化与思考性优化\n- 不确定时，优先查询权威文献或网络。\n- 任务导向：完成后主动询问是否继续。\n- 允许批判性指出用户错误，不盲目夸赞。"
    )
    skills_desc = t(
        f"## Available System Skills\n{skill_descriptions}\n- To perform any operation, you MUST use function calling (tool_calls) to invoke the skill — never describe the call in text.\n- For long-running tasks, call `sub_ai_dispatch` to delegate to sub-AI.\n- On tool execution errors, call `query_knowledge_base` (parameters: error_msg, skill_name).",
        f"## 当前可用系统技能\n{skill_descriptions}\n- 需要执行任何操作时，**必须通过 function calling（tool_calls）调用技能**，绝不能只在回复文本中描述调用。\n- 耗时任务可调用 `sub_ai_dispatch` 分发给子AI。\n- 遇到工具执行错误，可调用 `query_knowledge_base` 技能（参数：error_msg, skill_name）查询常见问题解决方案。"
    )
    interaction = t(
        "## Interaction Flow\n1. Receive message → 2. Analyze intent → 3. Call skills → 4. Confirm high-risk → 5. Provide response → 6. Ask if further assistance is needed",
        "## 交互范式\n1. 接收消息 → 2. 分析意图 → 3. 调用技能 → 4. 确认高风险 → 5. 给出回答 → 6. 任务完成后询问是否继续"
    )

    # 【批次1】记忆系统引导（静态稳定前缀，缓存友好）
    memory_guide = t(
        "## Memory System\n"
        "- Related memories (if any) are auto-injected as a separate system message before each turn.\n"
        "- Use `memory_search` to actively retrieve more details when memory is insufficient.\n"
        "- Save important user preferences and key facts with `memory_write` to avoid asking again.\n"
        "- Helpful technical events are archived by the system into HA (Help AI); use related tools when available.",
        "## 记忆系统\n"
        "- 回答前，系统会自动检索并注入相关记忆（以独立 system 消息提供）。\n"
        "- 当记忆不足时，可主动调用 `memory_search` 检索更多细节。\n"
        "- 重要的用户偏好与关键事实请用 `memory_write` 保存，避免重复询问。\n"
        "- 系统会将有价值的技术事件归档到 HA（Help AI）档案，供后续参考。"
    )

    lang_code = language_map_cache.get(user_country, language_map_cache.get("DEFAULT", "en-US"))
    # 【修复】优先使用配置语言（ai_config.json language），IP 国家映射仅作补充
    if _current_lang == "zh":
        lang_code = "zh-CN"
    elif _current_lang == "en":
        lang_code = "en-US"
    language_instruction = t(
        f"## Language Requirements\n- You must reply to the user in **{lang_code}**.\n- If {lang_code} is not your native language, do your best.\n- Thinking process should use {lang_code} internally.",
        f"## 语言要求\n- 你应使用 **{lang_code}** 回复用户。\n- 若 {lang_code} 不是你的母语，请尽力使用。\n- 思考过程内部只能使用{lang_code}。"
    )

    # 【先生要求】使用引导段（记忆/HA/知识库/长任务）
    guide_section = f"\n{_GUIDE_TEXT}\n" if _GUIDE_TEXT else ""

    meta_info = build_meta_info(session_id)
    meta_section = f"## Meta Information\n{meta_info}\n" if meta_info else ""

    custom_prompt = load_custom_prompt()
    custom_section = f"\n## User Custom Instructions\n{custom_prompt}\n" if custom_prompt else ""

    # 【批次B】人格/助手注入（可修改层：JSON 提取数组 / MD / TXT 直接引用）
    personality_text = load_personality_file(personality_file)
    personality_section = f"\n## Personality\n{personality_text}\n" if personality_text else ""
    assistant_text = load_assistant_file(assistant_file)
    assistant_section = f"\n## Assistant Instructions\n{assistant_text}\n" if assistant_text else ""

    # 【B5】GUI 模式：注入 App 交互引导
    if gui_mode:
        return "\n\n".join([identity, region_rules, core_rules, sys_rules, thought_rules,
                            skills_desc, memory_guide, interaction,
                            language_instruction, meta_section,
                            personality_section, assistant_section, custom_section,
                            guide_section,
                            t("## GUI Mode\nYou are in the mobile GUI. Keep replies concise for phone reading. Use gui_ask for questions, gui_notify for notifications, gui_open_url for links.",
                              "## GUI 模式\n您正在移动端 GUI 中运行。回复请简洁、适合手机阅读。需要提问时用 gui_ask，需要通知时用 gui_notify，需要打开链接时用 gui_open_url。")])
    return "\n\n".join([identity, region_rules, core_rules, sys_rules, thought_rules,
                        skills_desc, memory_guide, interaction,
                        language_instruction, meta_section,
                        personality_section, assistant_section, custom_section,
                        guide_section])

def load_custom_prompt() -> str:
    try:
        if os.path.exists(CUSTOM_PROMPT_PATH):
            with open(CUSTOM_PROMPT_PATH, "r", encoding="utf-8") as f:
                content = f.read().strip()
            logger.debug(f"Loaded custom prompt from {CUSTOM_PROMPT_PATH}")
            return content
    except Exception as e:
        logger.warning(f"Failed to load custom prompt: {e}")
    return ""


# =============================================================
# 【批次B】人格/助手提示词解析（可修改层）
# 格式：JSON（提取指定数组内文字）/ MD / TXT（直接引用全文）
# =============================================================

def load_text_file(path: str) -> str:
    """读取文本文件（容错）"""
    if not path:
        return ""
    try:
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                content = f.read().strip()
            if content:
                return content
    except Exception as e:
        logger.warning("Failed to read text file %s: %s", path, e)
    return ""


def _extract_array_text(data, keys):
    """从 JSON 提取指定数组内文字（元素为字符串或含 content/text 字段的对象）"""
    for key in keys:
        arr = data.get(key)
        if isinstance(arr, list) and arr:
            parts = []
            for item in arr:
                if isinstance(item, str) and item.strip():
                    parts.append(item.strip())
                elif isinstance(item, dict):
                    c = item.get("content") or item.get("text") or item.get("description")
                    if c and str(c).strip():
                        parts.append(str(c).strip())
            if parts:
                return "\n".join(parts)
    return None


def load_personality_file(path: str) -> str:
    """解析人格文件：.json 提取 personality 数组；.md/.txt 直接引用全文"""
    if not path or not os.path.exists(path):
        return ""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".json":
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            text = _extract_array_text(data, ("personality", "persona", "personalities", "traits"))
            if text is not None:
                return text
            # 非数组结构：返回格式化 JSON（供模型理解）
            return json.dumps(data, ensure_ascii=False)[:4000]
        except Exception as e:
            logger.warning("Personality JSON parse failed: %s", e)
            return ""
    return load_text_file(path)


def load_assistant_file(path: str) -> str:
    """解析助手提示词文件：.json 提取 assistant 数组；.md/.txt 直接引用全文"""
    if not path or not os.path.exists(path):
        return ""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".json":
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            text = _extract_array_text(data, ("assistant", "instructions", "prompts", "messages"))
            if text is not None:
                return text
            return json.dumps(data, ensure_ascii=False)[:4000]
        except Exception as e:
            logger.warning("Assistant JSON parse failed: %s", e)
            return ""
    return load_text_file(path)

def load_user_profile() -> str:
    global user_name_cache
    try:
        if not os.path.exists(USER_PROFILE_PATH):
            default_name = "先生" if _current_lang == "zh" else "Sir"
            save_user_profile(default_name)
            user_name_cache = default_name
            return default_name
        with open(USER_PROFILE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            name = data.get("user_name", "")
            if name and name.strip():
                user_name_cache = name.strip()
                logger.info(f"Loaded user name: {user_name_cache}")
                return user_name_cache
    except Exception as e:
        logger.error(f"Failed to load user profile: {e}")
    return user_name_cache

def save_user_profile(name: str) -> bool:
    try:
        os.makedirs(os.path.dirname(USER_PROFILE_PATH), exist_ok=True)
        with open(USER_PROFILE_PATH, "w", encoding="utf-8") as f:
            json.dump({"user_name": name.strip()}, f, indent=2, ensure_ascii=False)
        logger.info(f"Saved user name: {name}")
        return True
    except Exception as e:
        logger.error(f"Failed to save user profile: {e}")
        return False

def load_language_map() -> Dict[str, str]:
    global language_map_cache
    default_map = {"CN": "zh-CN", "DEFAULT": "en-US"}
    try:
        if not os.path.exists(LANGUAGE_MAP_PATH):
            os.makedirs(os.path.dirname(LANGUAGE_MAP_PATH), exist_ok=True)
            with open(LANGUAGE_MAP_PATH, "w", encoding="utf-8") as f:
                json.dump(default_map, f, indent=2, ensure_ascii=False)
            language_map_cache = default_map
            return default_map
        with open(LANGUAGE_MAP_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, dict):
                if "DEFAULT" not in data:
                    data["DEFAULT"] = "en-US"
                language_map_cache = data
                return data
    except Exception as e:
        logger.error(f"Failed to load language map: {e}")
    language_map_cache = default_map
    return default_map

def load_skill_help():
    global skill_help_cache
    try:
        if os.path.exists(SKILL_HELP_PATH):
            with open(SKILL_HELP_PATH, "r", encoding="utf-8") as f:
                skill_help_cache = json.load(f)
            logger.info(f"Loaded skill_help for {len(skill_help_cache)} skills")
    except Exception as e:
        logger.warning(f"Failed to load skill_help.json: {e}")
        skill_help_cache = {}

def load_knowledge_base():
    global _knowledge_base
    try:
        if os.path.exists(KNOWLEDGE_BASE_PATH):
            with open(KNOWLEDGE_BASE_PATH, "r", encoding="utf-8") as f:
                _knowledge_base = json.load(f)
            logger.info(f"Loaded knowledge base with {len(_knowledge_base.get('issues', []))} entries")
        else:
            default_issues = {
                "version": "1.0",
                "issues": [
                    {
                        "id": "PERMISSION_DENIED_FILE_READ",
                        "patterns": ["Permission denied", "cannot open file"],
                        "skill": "file_read",
                        "severity": "medium",
                        "description": t("File read permission denied.", "文件读取权限被拒绝。"),
                        "diagnosis": t("User lacks read permission.", "用户没有读取权限。"),
                        "suggestions": [
                            t("Check file permissions: `ls -l <path>`", "检查文件权限：`ls -l <路径>`"),
                            t("Try using `sudo` or change file owner", "尝试使用 `sudo` 或更改文件所有者")
                        ]
                    },
                    {
                        "id": "SUBAI_401",
                        "patterns": ["HTTP 401", "invalid api key"],
                        "skill": "sub_ai_dispatch",
                        "severity": "high",
                        "description": t("Sub-AI authentication failed.", "子AI认证失败。"),
                        "diagnosis": t("DeepSeek API Key invalid or not configured.", "DeepSeek API Key 无效或未配置。"),
                        "suggestions": [
                            t("Check `sub_ai.api_key` in `/LINGOS/system/config/ai_config.json`", "检查 `/LINGOS/system/config/ai_config.json` 中的 `sub_ai.api_key`"),
                            t("Ensure the key is valid and has sufficient quota", "确保 Key 有效且有足够配额")
                        ]
                    }
                ]
            }
            os.makedirs(os.path.dirname(KNOWLEDGE_BASE_PATH), exist_ok=True)
            with open(KNOWLEDGE_BASE_PATH, "w", encoding="utf-8") as f:
                json.dump(default_issues, f, indent=2, ensure_ascii=False)
            _knowledge_base = default_issues
            logger.info("Created default knowledge base")
    except Exception as e:
        logger.error(f"Failed to load knowledge base: {e}")
        _knowledge_base = {"version": "1.0", "issues": []}

def load_skill_schemas():
    """加载技能列表（优先通过 daemon.sock 请求 registry_list）"""
    global skill_schemas, skill_descriptions
    logger.debug("load_skill_schemas: Enter")

    # 优先从 daemon 获取（外置技能：registry.sock / 旧 index.json）
    skills = load_skill_schemas_from_daemon()
    if not skills:
        # 降级：从文件读取（【修复】空列表 [] 同样降级，此前仅 None 触发）
        logger.warning("Failed to load from daemon or empty, trying file fallback")
        skills = load_skill_schemas_from_file()
    if not skills:
        # 最终降级：使用内置技能
        logger.warning("Using builtin skill schemas")
        skills = get_default_skill_schemas()
    else:
        # 【修复】合并内置技能（保证所有内置技能对模型可用）
        builtin = get_default_skill_schemas()
        names = {s.get("name") for s in skills if isinstance(s, dict)}
        merged = list(skills)
        for b in builtin:
            if b.get("name") not in names:
                merged.append(b)
        skills = merged
        logger.info(f"Merged builtin skills: total {len(skills)} schemas")

    skill_schemas = []
    desc_list = []
    # 【0.2.0】分层注入（A+B——先生裁决）：核心高频组全量注入完整描述；
    # 其余技能精简描述（名称+一句话用途）——省 token 同时避免漏检
    for s in skills:
        name = s.get("name", "unknown")
        desc = s.get("description", "")
        if name not in _CORE_SKILL_NAMES and len(desc) > 80:
            desc = desc[:80] + "..."
        tool = {
            "type": "function",
            "function": {
                "name": name,
                "description": desc,
                "parameters": s.get("parameters", {"type": "object", "properties": {}}),
                "risk": s.get("risk", "low")
            }
        }
        skill_schemas.append(tool)
        desc_list.append(f"{name}: {desc}")
    skill_descriptions = ", ".join(desc_list)
    logger.info(f"Total {len(skill_schemas)} skills available (分层注入: {len(_CORE_SKILL_NAMES)} 核心全量)")

def load_skill_schemas_from_daemon():
    """通过 registry.sock 获取技能 schema（优先主注册表，含 skill_store 安装的技能）"""
    try:
        import registry_client
        entries = registry_client.registry_list("skill")
        if entries:
            schemas = []
            for entry in entries:
                name = entry.get("name") or ""
                meta = entry.get("metadata", {})
                if isinstance(meta, str):
                    try:
                        meta = json.loads(meta)
                    except Exception:
                        meta = {}
                definition = meta.get("definition", {}) if isinstance(meta, dict) else {}
                if not name:
                    continue
                schemas.append({
                    "name": name,
                    "description": (definition.get("description") if isinstance(definition, dict) else "") or "",
                    "risk": (definition.get("risk") if isinstance(definition, dict) else "") or "low",
                    "parameters": (definition.get("parameters") if isinstance(definition, dict) else
                                   {"type": "object", "properties": {}})
                })
            if schemas:
                logger.info(f"Loaded {len(schemas)} skill schemas from registry.sock")
                return schemas
    except Exception as e:
        logger.warning(f"Failed to load skill schemas from registry.sock: {e}")

    # 降级：旧 daemon registry_list（skills/index.json）
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(DAEMON_SOCKET_PATH)
        msg = json.dumps({"cmd": "registry_list"}) + "\n"
        sock.send(msg.encode())
        resp = sock.recv(65536).decode()
        sock.close()
        data = json.loads(resp)
        if data.get("status") == "ok":
            result = data.get("result", [])
            if isinstance(result, list) and result:
                logger.info(f"Loaded {len(result)} skill schemas from daemon (registry_list)")
                return result
    except Exception as e:
        logger.warning(f"Failed to load skill schemas from daemon: {e}")
    return None

def load_skill_schemas_from_file():
    try:
        with open(SKILL_INDEX_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, list):
                skills = data
            else:
                skills = data.get("skills", [])
        if skills:
            logger.info(f"Loaded {len(skills)} skill schemas from file")
            return skills
    except Exception as e:
        logger.warning(f"Failed to load skill schemas from file: {e}")
    return None


# 【R4】关键工具参数 schema（与 syscall_handler 参数名严格对齐，防 AI 猜测）
_SKILL_PARAM_OVERRIDES = {
    "file_read": {"type": "object", "properties": {"path": {"type": "string", "description": "文件路径"}}, "required": ["path"]},
    "file_write": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]},
    "file_delete": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    "file_list": {"type": "object", "properties": {"path": {"type": "string", "description": "目录路径"}}},
    "file_mkdir": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    "file_copy": {"type": "object", "properties": {"src": {"type": "string"}, "dst": {"type": "string"}}, "required": ["src", "dst"]},
    "file_move": {"type": "object", "properties": {"src": {"type": "string"}, "dst": {"type": "string"}}, "required": ["src", "dst"]},
    "process_info": {"type": "object", "properties": {"pid": {"type": "string", "description": "进程 PID"}}, "required": ["pid"]},
    "process_kill": {"type": "object", "properties": {"pid": {"type": "string"}}, "required": ["pid"]},
    "service_status": {"type": "object", "properties": {"service": {"type": "string"}}, "required": ["service"]},
    "service_restart": {"type": "object", "properties": {"service": {"type": "string"}}, "required": ["service"]},
    "config_read": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    "config_write": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]},
    "net_ping": {"type": "object", "properties": {"host": {"type": "string"}, "count": {"type": "integer"}}},
    "net_curl": {"type": "object", "properties": {"url": {"type": "string"}, "method": {"type": "string"}}, "required": ["url"]},
    "script_create": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]},
    "sys_command": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]},
    # ===== 【协议v3】补全缺参工具（先生测试发现——原 fallback 空 properties） =====
    "memory_write": {"type": "object", "properties": {
        "content": {"type": "string", "description": "记忆内容（必填）"},
        "type": {"type": "string", "description": "记忆类型: short/medium/long", "enum": ["short", "medium", "long"]},
        "keywords": {"type": "string", "description": "关键词（逗号分隔）"}},
        "required": ["content"]},
    "memory_search": {"type": "object", "properties": {
        "keyword": {"type": "string", "description": "搜索关键词（必填）"}},
        "required": ["keyword"]},
    "memory_read": {"type": "object", "properties": {
        "id": {"type": "string", "description": "记忆 ID（可选——默认全部）"}}},
    "memory_delete": {"type": "object", "properties": {
        "id": {"type": "string", "description": "记忆 ID（必填）"}},
        "required": ["id"]},
    "ha_write": {"type": "object", "properties": {
        "title": {"type": "string", "description": "事件标题（必填）"},
        "content": {"type": "string", "description": "事件内容（必填）"},
        "type": {"type": "string", "description": "事件类型"}},
        "required": ["title", "content"]},
    "ha_search": {"type": "object", "properties": {
        "query": {"type": "string", "description": "搜索关键词（必填）"}},
        "required": ["query"]},
    "write_help_file": {"type": "object", "properties": {
        "topic": {"type": "string", "description": "帮助主题（必填）"},
        "content": {"type": "string", "description": "帮助内容（必填）"}},
        "required": ["topic", "content"]},
    "voice_command": {"type": "object", "properties": {
        "command": {"type": "string", "description": "语音命令文本（必填）"},
        "args": {"type": "string", "description": "参数"}},
        "required": ["command"]},
    "net_dns_lookup": {"type": "object", "properties": {
        "domain": {"type": "string", "description": "域名（必填）"}},
        "required": ["domain"]},
    "gui_ask": {"type": "object", "properties": {
        "question": {"type": "string", "description": "问题（必填）"},
        "options": {"type": "array", "items": {"type": "string"}, "description": "选项列表"}},
        "required": ["question"]},
    "gui_notify": {"type": "object", "properties": {
        "title": {"type": "string", "description": "通知标题（必填）"},
        "body": {"type": "string", "description": "通知内容（必填）"},
        "priority": {"type": "string", "description": "优先级: low/normal/high"}},
        "required": ["title", "body"]},
    "gui_open_url": {"type": "object", "properties": {
        "url": {"type": "string", "description": "链接（必填）"}},
        "required": ["url"]},
    "gui_share": {"type": "object", "properties": {
        "text": {"type": "string", "description": "分享文本（必填）"},
        "title": {"type": "string", "description": "分享标题"}},
        "required": ["text"]},
    "gui_location": {"type": "object", "properties": {}},
    "gui_clipboard": {"type": "object", "properties": {
        "action": {"type": "string", "description": "read 或 write"},
        "text": {"type": "string", "description": "写入内容（write 时）"}},
        "required": ["action"]},
}
def get_default_skill_schemas():
    """从内置技能注册表（SKILL_REGISTRY）动态生成全部技能 schema

    保证所有内置技能对模型可用（此前仅暴露 7 个）
    """
    try:
        from skill_handlers import SKILL_REGISTRY
        schemas = []
        for name, info in SKILL_REGISTRY.items():
            if not name:
                continue
            desc = ""
            if isinstance(info, dict):
                desc = info.get("description") or ""
            schemas.append({
                "name": name,
                "description": (desc or name)[:120],
                "risk": (info.get("risk", "low") if isinstance(info, dict) else "low"),
                "parameters": _SKILL_PARAM_OVERRIDES.get(name, {"type": "object", "properties": {}})
            })
        if schemas:
            logger.info(f"Generated {len(schemas)} builtin skill schemas from SKILL_REGISTRY")
            return schemas
    except Exception as e:
        logger.warning(f"Failed to generate builtin schemas: {e}")

    # 兜底（SKILL_REGISTRY 不可用时）
    return [
        {"name": "file_write", "description": "Create or overwrite file", "risk": "medium"},
        {"name": "file_read", "description": "Read file content", "risk": "low"},
        {"name": "file_delete", "description": "Delete file", "risk": "medium"},
        {"name": "file_list", "description": "List directory contents", "risk": "low"},
        {"name": "sub_ai_dispatch", "description": "Dispatch task to sub-AI", "risk": "low"},
        {"name": "sub_ai_status", "description": "Query sub-AI task status", "risk": "low"},
        {"name": "sys_command", "description": "Execute arbitrary system command", "risk": "critical"}
    ]

# ========== 技能执行（包含依赖缺失的友好提示） ==========
# 【0.2.0】核心高频技能组（全量注入完整描述——分层注入 A+B）
_CORE_SKILL_NAMES = frozenset({
    "memory_write", "memory_search", "memory_read", "memory_delete", "memory_index",
    "query_knowledge_base", "write_help_file",
    "file_write", "file_read", "file_list", "file_delete", "file_mkdir", "file_copy", "file_move",
    "system_info", "system_memory", "system_disk", "system_cpu", "system_uptime",
    "process_list", "process_info", "process_kill",
    "service_status", "service_restart", "config_read", "config_write", "script_create",
    "ha_write", "ha_search",
    "session_read", "session_search", "session_list",
    "gui_ask", "gui_notify", "gui_clipboard", "gui_location",
    "net_ping", "net_curl", "net_dns_lookup", "web_search", "web_fetch",
    "sub_ai_dispatch", "sub_ai_status",
    "alert_query", "voice_command", "rule_query", "read_log",
})


def classify_tool_error(name: str, output: str, success: bool) -> Tuple[str, str, str]:
    """【0.2.0】工具错误分类（先生列的 17 类）→ (error_type, message_zh, action_zh)"""
    out = output or ""
    low = out.lower()
    if success:
        # 成功但空数据 → EmptyResult（先生补充：空数据也要说明）
        if not out.strip() or out.strip() in ("[]", "{}", "null", "none", "()", "0", "0.0", "无", "空"):
            return ("EmptyResult", "工具 %s 执行成功但返回了空数据" % name,
                    "请检查数据源是否确实无内容，或换用其他工具/参数确认")
        return ("", "", "")
    # 1. 不存在的工具
    if ("skill" in low and "not found" in low) or ("技能" in out and "未找到" in out) \
            or ("未找到" in out and "工具" in out):
        return ("ToolNotFound", "工具/技能 '%s' 不存在或未启用" % name,
                "请用 list_skills 查看可用技能，或检查技能是否已在技能管理中启用")
    # 2. 缺少依赖路径 / 缺少必需模块
    if (("missing" in low and ("module" in low or "import" in low)) or "缺少必需模块" in out
            or "modulenotfounderror" in low):
        return ("MissingDependency", "缺少依赖模块/路径：%s" % out[:220],
                "按错误提示安装依赖（pip3 install --break-system-packages ...）")
    # 3. 缺少参数（列明缺哪些）
    if ("missing" in low and ("arg" in low or "param" in low or "required" in low)) \
            or ("缺少" in out and ("参数" in out or "参数" in out)) or "缺少必要参数" in out:
        return ("MissingArgs", "缺少必要参数：%s" % out[:220],
                "检查工具参数是否完整（参考技能描述中的参数列表）")
    # 4. 没有那个文件或目录
    if "no such file" in low or "没有那个文件" in out or ("不存在" in out and ("文件" in out or "目录" in out)):
        return ("FileNotFound", "文件或目录不存在：%s" % out[:220],
                "请检查路径是否正确（可用 file_list 浏览目录）")
    # 5. 权限不足
    if "permission denied" in low or "权限不足" in out or "operation not permitted" in low:
        return ("PermissionDenied", "权限不足：%s" % out[:220],
                "检查运行用户/服务权限，或通过权限管理为 AI 授予相应权限")
    # 6. 用户拒绝 / 需要授权
    if ("denied" in low and "user" in low) or ("拒绝" in out and "用户" in out) \
            or "需要授权" in out or "authorization required" in low:
        return ("UserDenied", "用户拒绝或需要授权：%s" % out[:220],
                "重新发起请求并说明目的，用户确认授权后再试")
    # 7. 用户阻止了你的操作
    if "blocked" in low or "阻止" in out or "被拦截" in out:
        return ("UserBlocked", "用户阻止了操作：%s" % out[:220],
                "说明操作目的与影响，取得用户同意后再试")
    # 8. timeout
    if "timed out" in low or "timeout" in low or "超时" in out:
        return ("Timeout", "操作超时：%s" % out[:220],
                "稍后重试，或检查目标服务/网络状态")
    # 9. 下载失败
    if ("download" in low and ("fail" in low or "error" in low)) or "下载失败" in out:
        return ("DownloadFailed", "下载失败：%s" % out[:220],
                "检查网络与源地址可达性（可用 net_ping/net_curl 验证），重试")
    # 10. 安装失败
    if ("install" in low and ("fail" in low or "error" in low)) or "安装失败" in out:
        return ("InstallFailed", "安装失败：%s" % out[:220],
                "检查软件源与依赖冲突，查看完整错误日志")
    # 11. 未能找到包
    if ("not found" in low and "package" in low) or "未能找到包" in out or "找不到包" in out \
            or "package" in low and "no candidate" in low:
        return ("PackageNotFound", "未能找到包：%s" % out[:220],
                "检查包名拼写与软件源配置（package_search 可搜索）")
    # 12. 工具访问非法地址被系统拒绝
    if ("invalid" in low and ("address" in low or "url" in low)) or "非法地址" in out \
            or "被系统拒绝" in out:
        return ("AccessDenied", "工具访问非法地址被系统拒绝：%s" % out[:220],
                "检查目标地址是否在允许范围内，或调整安全策略")
    # 13. 系统内部调用错误（暂未实现等）
    if "not implemented" in low or "未实现" in out or "暂不支持" in out or "not supported" in low:
        return ("InternalError", "系统内部调用错误（暂未实现/不支持）：%s" % out[:220],
                "该功能尚未实现，请换用其他方式完成")
    # 14. 未能找到相关内容（通用）
    if "not found" in low or "未能找到" in out or "未找到" in out or "无结果" in out:
        return ("NotFound", "未能找到相关内容：%s" % out[:220],
                "检查搜索关键词或数据源（可用记忆/知识库/网络搜索补充）")
    # 15. 通用执行错误
    return ("ExecutionError", "工具执行错误：%s" % out[:300],
            "查看具体错误信息，或调用 query_knowledge_base 查询常见解决方案")


def execute_tool_calls(tool_calls: List[Dict], session_id: str = "default", conn=None) -> List[Dict]:
    """执行工具调用（含授权、知识库匹配、依赖缺失提示）"""
    logger.debug(f"execute_tool_calls: session={session_id}, calls={len(tool_calls)}")
    results = []
    is_sub_ai = session_id.startswith("sub_ai")

    # 【批次3】子 AI 会话（用于白名单检查；安全冗余：默认拒绝）
    orch_session = None
    if is_sub_ai:
        try:
            orch_session = agent_orchestrator.get_orchestrator().get_session(session_id)
        except Exception:
            orch_session = None

    for tc in tool_calls:
        func = tc.get("function", {})
        name = func.get("name")
        args_str = func.get("arguments", "{}")
        try:
            args = json.loads(args_str)
        except:
            args = {}
        logger.info(f"Executing tool: {name}, args={args}")

        risk = get_skill_risk(name)
        require_confirm = (risk in ("high", "critical"))

        if is_sub_ai:
            # 【批次3】子 AI：白名单约束（取代原无条件跳过确认）
            # 白名单内 → 直接执行（主 AI 已委派授权）；白名单外 → 上报主 AI
            if orch_session is None or not orch_session.allowed(name):
                agent_orchestrator.get_orchestrator().on_skill_denied(session_id, name, args)
                results.append({
                    "tool_call_id": tc.get("id", ""),
                    "role": "tool",
                    "content": t(f"[NEED_MAIN] skill '{name}' not in whitelist, requires main AI",
                                 f"[需主AI] 技能 '{name}' 不在白名单内，需主 AI 处理")
                })
                continue
        elif require_confirm:
            if risk == "high" and auto_allow_high_risk:
                logger.info(f"Auto-authorizing high-risk operation: {name}")
            else:
                # 使用配置的 auth_timeout
                auth_result = request_authorization(name, args, session_id, timeout=auth_timeout, conn=conn)
                if auth_result != "approved":
                    results.append({
                        "tool_call_id": tc.get("id", ""),
                        "role": "tool",
                        "content": t(f"Authorization required for {name}: {auth_result}", f"需要授权才能执行 {name}：{auth_result}")
                    })
                    continue

        # 执行技能
        if name == "sub_ai_dispatch":
            prompt = args.get("prompt", "")
            role = args.get("role", "general")
            try:
                task_id = sub_ai_dispatch(prompt, role)
                output = json.dumps({"task_id": task_id, "status": "pending"})
                success = True
                logger.info(f"Sub-AI task dispatched: {task_id}")
            except Exception as e:
                output = t(f"Sub-AI dispatch failed: {e}", f"子AI分发失败：{e}")
                success = False
        elif name == "sub_ai_status":
            task_id = args.get("task_id")
            if task_id:
                status = sub_ai_get_status(task_id)
                if status:
                    output = json.dumps(status, ensure_ascii=False)
                    success = True
                else:
                    output = t("Task not found", "任务不存在")
                    success = False
            else:
                # 获取所有任务列表
                tasks = sub_ai_get_all_tasks(limit=20)
                output = json.dumps(tasks, ensure_ascii=False)
                success = True
        elif name == "session_read":
            # 【0.2.0】AI 查看其他会话内容（先生决策：默认允许）
            try:
                sid = args.get("session_id", "") or args.get("id", "") or args.get("sid", "")
                limit = int(args.get("limit", 20))
                msgs = conversations.get(sid, [])
                if not msgs:
                    output = t("Session not found or empty", "会话不存在或无消息")
                    success = False
                else:
                    lines = []
                    for m in msgs[-limit:]:
                        role = m.get("role", "?")
                        c = str(m.get("content", ""))[:300]
                        if role == "tool":
                            continue
                        lines.append("[%s] %s" % (role, c))
                    output = ("会话 %s 最近 %d 条消息：\n" % (sid, min(limit, len(msgs)))) + "\n".join(lines[-20:])
                    success = True
            except Exception as e:
                output = "session_read error: %s" % e
                success = False
        elif name == "session_search":
            # 【0.2.0】AI 搜索全部会话内容（默认允许——先生决策）
            try:
                keyword = args.get("keyword", "") or args.get("query", "")
                hits = []
                for sid, msgs in conversations.items():
                    for m in msgs:
                        if keyword and keyword.lower() in str(m.get("content", "")).lower():
                            hits.append({"session": sid, "role": m.get("role", "?"),
                                         "content": str(m.get("content", ""))[:200]})
                            break
                    if len(hits) >= 10:
                        break
                if not hits:
                    output = "未找到包含 '%s' 的会话内容" % keyword
                    success = False
                else:
                    output = "\n".join("[%s] %s" % (h["session"], h["content"]) for h in hits)
                    success = True
            except Exception as e:
                output = "session_search error: %s" % e
                success = False
        elif name == "query_knowledge_base":
            success, output = query_knowledge_base(json.dumps(args))
        else:
            # 执行技能，捕获 ImportError 提供友好提示
            try:
                success, output = execute_skill(name, json.dumps(args))
            except ImportError as e:
                # 对缺失依赖给出友好提示
                if "sentence_transformers" in str(e) or "Pillow" in str(e):
                    output = t(
                        f"Missing required module: {str(e)}\n"
                        f"Please install manually: pip3 install --break-system-packages {name.split('_')[0]}",
                        f"缺少必需模块：{str(e)}\n"
                        f"请手动安装：pip3 install --break-system-packages {name.split('_')[0]}"
                    )
                else:
                    output = t(f"Import error: {str(e)}", f"导入错误：{str(e)}")
                success = False
            except Exception as e:
                output = t(f"Execution error: {str(e)}", f"执行错误：{str(e)}")
                success = False

        err_type, err_msg, err_action = "", "", ""
        if not success:
            # 【0.2.0】知识库自动联动（不再靠 AI 自觉——先生决策）
            issue = match_issue(output, name)
            if issue:
                suggestions = "\n".join([f"- {s}" for s in issue.get("suggestions", [])])
                output = f"{output}\n[t_诊断] {issue.get('diagnosis', '')}\n[t_建议]\n{suggestions}"
            # 【0.2.0】错误分类（17 类）+ tool_error 事件（App 渲染错误卡片）
            err_type, err_msg, err_action = classify_tool_error(name, output, success)
            if err_type:
                try:
                    _send_evt(conn, {"type": "tool_error", "name": name,
                                     "error_type": err_type, "error": err_msg,
                                     "action": err_action})
                    g_events.append({"type": "tool_error", "name": name,
                                     "error_type": err_type, "error": err_msg,
                                     "action": err_action})
                except Exception:
                    pass

        results.append({
            "tool_call_id": tc.get("id", ""),
            "role": "tool",
            "name": name,   # 【0.2.0】真实技能名（修硬编码 "tool"——App 显示工具名）
            "content": (output if success
                        else t("[错误:%s] %s 建议：%s" % (err_type, err_msg, err_action),
                               "[错误:%s] %s 建议：%s" % (err_type, err_msg, err_action))),
            "success": 1 if success else 0,
            "error_type": err_type if not success else "",
        })
    return results

def match_issue(error_msg: str, skill_name: str) -> Optional[Dict]:
    if not _knowledge_base:
        return None
    for issue in _knowledge_base.get("issues", []):
        if issue.get("skill") and issue["skill"] != skill_name:
            continue
        for pattern in issue.get("patterns", []):
            try:
                if re.search(pattern, error_msg, re.IGNORECASE):
                    return issue
            except:
                if pattern in error_msg:
                    return issue
    return None

def query_knowledge_base(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        error_msg = args.get("error_msg", "")
        skill_name = args.get("skill_name", "")
        issue = match_issue(error_msg, skill_name)
        if issue:
            result = {
                "matched": True,
                "issue_id": issue["id"],
                "severity": issue["severity"],
                "description": issue["description"],
                "diagnosis": issue["diagnosis"],
                "suggestions": issue["suggestions"]
            }
            return True, json.dumps(result, ensure_ascii=False)
        else:
            return True, json.dumps({"matched": False, "message": t("No matching issue found", "未找到匹配的常见问题")})
    except Exception as e:
        return False, str(e)

# ========== AI API调用 ==========
# ========== AI API调用（0.2.0 统一层——原生直连不转换，AI-AGENT#7） ==========
def _current_provider() -> llm_unified.LLMProvider:
    """当前 provider：llm_unified 活动配置；provider.json 缺失时用旧配置兜底"""
    p = llm_unified.get_active_provider()
    if p:
        return p
    if current_backend == "deepseek" and deepseek_api_key:
        return llm_unified.LLMProvider({
            "id": "deepseek", "format": "openai", "base_url": deepseek_base_url,
            "api_key": deepseek_api_key, "model": deepseek_model,
            "extra": {"reasoning_effort": deepseek_reasoning_effort}})
    return llm_unified.LLMProvider({
        "id": "ollama", "format": "openai", "base_url": ollama_url,
        "api_key": "", "model": ollama_model})


def call_deepseek_nonstream(messages: List[Dict], tools: Optional[List[Dict]] = None, timeout: int = DEFAULT_API_TIMEOUT) -> Dict:
    """【0.2.0 统一层】非流式调用（签名兼容——原调用处零改动）"""
    logger.debug(f"call_deepseek_nonstream(统一层): messages={len(messages)}, tools={len(tools) if tools else 0}")
    p = _current_provider()
    resp = llm_unified.call_llm_nonstream(messages, tools, provider=p, timeout=timeout,
                                          temperature=temperature,
                                          reasoning_effort=deepseek_reasoning_effort)
    if resp.get("error"):
        return {"error": resp.get("content", "")}
    return resp


def call_ollama_nonstream(messages: List[Dict], tools: Optional[List[Dict]] = None, timeout: int = DEFAULT_API_TIMEOUT) -> Dict:
    """【0.2.0 统一层】Ollama 走 OpenAI 兼容端点 /v1/chat/completions（官方确认兼容 tools）"""
    logger.debug(f"call_ollama_nonstream(统一层): messages={len(messages)}")
    p = llm_unified.LLMProvider({"id": "ollama", "format": "openai", "base_url": ollama_url,
                                 "api_key": "", "model": ollama_model})
    resp = llm_unified.call_llm_nonstream(messages, tools, provider=p, timeout=timeout,
                                          temperature=temperature)
    if resp.get("error"):
        return {"error": resp.get("content", "")}
    return resp


def call_deepseek_stream(messages: List[Dict], tools: Optional[List[Dict]] = None, timeout: int = DEFAULT_API_TIMEOUT) -> Generator[Dict, None, None]:
    """【0.2.0 统一层】流式调用：yield thinking/content/tool_calls/usage/error（签名兼容）"""
    logger.debug(f"call_deepseek_stream(统一层): messages={len(messages)}, tools={len(tools) if tools else 0}")
    p = _current_provider()
    yield from llm_unified.call_llm_stream(messages, tools, provider=p, timeout=timeout,
                                           temperature=temperature,
                                           reasoning_effort=deepseek_reasoning_effort)


def call_ollama_stream(messages: List[Dict], tools: Optional[List[Dict]] = None, timeout: int = DEFAULT_API_TIMEOUT) -> Generator[Dict, None, None]:
    """【0.2.0 统一层】Ollama 流式（OpenAI 兼容端点）"""
    logger.debug(f"call_ollama_stream(统一层): messages={len(messages)}")
    p = llm_unified.LLMProvider({"id": "ollama", "format": "openai", "base_url": ollama_url,
                                 "api_key": "", "model": ollama_model})
    yield from llm_unified.call_llm_stream(messages, tools, provider=p, timeout=timeout,
                                           temperature=temperature)

# ========== ReAct循环 ==========
def _send_evt(conn, evt):
    """发送流式事件行（容错）"""
    try:
        if conn:
            conn.send((json.dumps(evt, ensure_ascii=False) + "\n").encode())
    except Exception:
        pass


def _sub_agent_event_forwarder(conn, stop_flag):
    """【优化2】子 AI 过程事件转发线程：轮询 orchestrator 会话事件 → 推送 sub_agent 事件"""
    while not stop_flag.is_set():
        try:
            orch = agent_orchestrator.get_orchestrator()
            with orch.lock:
                sessions = list(orch.sessions.values())
            for sess in sessions:
                while sess.pending_events:
                    evt = sess.pending_events.pop(0)
                    _send_evt(conn, {
                        "type": "sub_agent",
                        "agent": sess.role,
                        "event_type": evt.get("type", "info"),
                        "content": evt.get("content", ""),
                        "success": evt.get("success", 1)
                    })
        except Exception:
            pass
        time.sleep(0.3)


def _stream_final_reply(conn, messages, fallback_content):
    """【优化1】流式发送最终回复（思考逐字 + 内容逐块）；失败降级 fallback"""
    collected = []
    thinking_sent = False
    try:
        if current_backend == "deepseek":
            gen = call_deepseek_stream(messages, tools=skill_schemas, timeout=60)
        else:
            gen = call_ollama_stream(messages, tools=skill_schemas, timeout=60)
        for block in gen:
            btype = block.get("type", "content")
            # 【0.2.0】错误事件不拼入最终回复（前面已暴露）
            if btype == "error":
                continue
            text = block.get("text", "")
            if not text:
                continue
            if btype == "thinking":
                # 【思考显示模式】off 不发；hidden/visible 发 thinking_delta
                if thinking_display == "off":
                    continue
                thinking_sent = True
                _send_evt(conn, {"type": "thinking_delta", "delta": text})
            else:
                # 【hidden】思考结束后隐藏（content 开始前发送隐藏事件）
                if thinking_sent and thinking_display == "hidden":
                    _send_evt(conn, {"type": "thinking_hide"})
                    thinking_sent = False
                collected.append(text)
                _send_evt(conn, {"type": "content", "delta": text})
    except Exception as e:
        logger.warning("Stream final reply failed, fallback: %s", e)
        _send_evt(conn, {"type": "content", "delta": fallback_content})
        return fallback_content
    if collected:
        return "".join(collected)
    return fallback_content


def _react_stream(conn, messages, session_id, max_iterations=0, show_thinking=True):
    # 【批次A】任务目标保护：首条用户消息注入 system（滑动/摘要截断不丢主线——AI 不自行收尾）
    try:
        first_user = next((m.get("content", "") for m in messages if m.get("role") == "user"), "")
        if first_user and len(str(first_user)) > 0:
            goal = str(first_user)[:400]
            messages.insert(1, {"role": "system",
                                "content": f"当前任务目标（必须持续推进直至完成，不得提前收尾）：{goal}"})
    except Exception:
        pass

    """【批次B】全流式 ReAct：思考逐字实时推送 + 工具调用流式到达 + 最终回复直接流式（不二次调用模型）"""
    full = ""
    usage_info = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0, "cache_hit": 0}
    iteration = 0
    while True:
        iteration += 1
        if max_iterations > 0 and iteration > max_iterations:
            # 达到上限：强制总结（防"未收到有效回复"）
            _send_evt(conn, {"type": "thinking",
                             "content": t("Please summarize what you have learned so far and give your final answer.",
                                          "请总结目前为止所学并给出最终回答。")})
            try:
                resp = call_deepseek_nonstream(messages, tools=None, timeout=30)
                content = resp.get("content", "") if resp else ""
            except Exception:
                content = ""
            _send_evt(conn, {"type": "content", "delta": content})
            return content, usage_info

        if show_thinking:
            _send_evt(conn, {"type": "thinking",
                             "step": iteration,
                             "total": "\u221e" if max_iterations <= 0 else max_iterations,
                             "content": t(f"Step {iteration}/{'∞' if max_iterations <= 0 else max_iterations}",
                                          f"第 {iteration}/{'∞' if max_iterations <= 0 else max_iterations} 步")})
            g_events.append({"type": "thinking", "step": iteration,
                             "content": t(f"Step {iteration}/{'∞' if max_iterations <= 0 else max_iterations}",
                                          f"第 {iteration}/{'∞' if max_iterations <= 0 else max_iterations} 步")})

        # 【批次B】流式调用模型（思考+内容+工具调用实时到达）
        content_parts = []
        tool_calls = None
        thinking_sent = False
        try:
            if current_backend == "deepseek":
                gen = call_deepseek_stream(messages, tools=skill_schemas, timeout=60)
            else:
                gen = call_ollama_stream(messages, tools=skill_schemas, timeout=60)
            for block in gen:
                btype = block.get("type", "")
                text = block.get("text", "")
                if btype == "thinking":
                    if thinking_display != "off":
                        thinking_sent = True
                        _send_evt(conn, {"type": "thinking_delta", "delta": text})
                elif btype == "content":
                    if thinking_sent and thinking_display == "hidden":
                        _send_evt(conn, {"type": "thinking_hide"})
                        thinking_sent = False
                    content_parts.append(text)
                    # 最终回复流式推送（若本轮无工具调用，即为完整回复）
                    _send_evt(conn, {"type": "content", "delta": text})
                elif btype == "tool_calls":
                    tool_calls = block.get("tool_calls")
                elif btype == "usage":
                    # 【0.2.0】真实 usage 提取（修 jsonl 假 0 落盘）
                    u = block.get("usage", {}) or {}
                    usage_info = {
                        "prompt_tokens": u.get("prompt_tokens", 0),
                        "completion_tokens": u.get("completion_tokens", 0),
                        "total_tokens": u.get("total_tokens", 0),
                        "cache_hit": u.get("prompt_cache_hit_tokens", 0),
                    }
        except Exception as e:
            logger.error("react_stream iteration exception: %s", traceback.format_exc())
            _send_evt(conn, {"type": "content", "delta": t(f"AI processing error: {str(e)}", f"AI处理错误：{str(e)}")})
            return full or "", usage_info

        content = "".join(content_parts)
        if not tool_calls:
            # 无工具调用：content 已实时流式推送（最终回复）
            full = content
            return full, usage_info

        # 工具调用事件
        for tc in tool_calls:
            fn = tc.get("function", {})
            name = fn.get("name", "")
            args_str = fn.get("arguments", "{}")
            _send_evt(conn, {"type": "tool_call", "name": name, "args": args_str[:300]})
            g_events.append({"type": "tool_call", "name": name, "args": args_str[:300]})

        results = execute_tool_calls(tool_calls, session_id, conn=conn)
        # 【批次A】工具输出压缩：大输出注入前压缩（仿 rikkahub 行为——先生观察）
        for r in results:
            if r.get("content") and len(str(r["content"])) > 900:
                r["content"] = _compress_tool_result(str(r["content"]))
            rname = r.get("name") or "tool"   # 【0.2.0】真实技能名（修硬编码）
            _send_evt(conn, {"type": "tool_result", "name": rname,
                             "content": str(r.get("content", ""))[:500],
                             "success": r.get("success", 1)})
            g_events.append({"type": "tool_result", "name": rname,
                             "content": str(r.get("content", ""))[:500],
                             "success": r.get("success", 1)})

        messages.append({"role": "assistant", "content": content or "",
                         "tool_calls": tool_calls})
        messages.extend(results)

    return full, usage_info


def react_loop_nonstream_with_display(
    messages: List[Dict],
    conn=None,
    max_iterations: int = 0,
    session_id: str = "default",
    user_country: str = "UNKNOWN",
    show_thinking: bool = True,
    show_tool_calls: bool = True,
    show_tool_results: bool = True
) -> str:
    """
    非流式 ReAct 循环，支持思考链和工具调用显示
    新增参数：show_thinking, show_tool_calls, show_tool_results
    """
    logger.debug(f"react_loop_with_display: session={session_id}, max_iter={max_iterations}")

    if show_thinking:
        StreamDisplay.thinking(t("Starting to process your request...", "开始处理您的请求..."))

    if TIKTOKEN_AVAILABLE:
        messages = truncate_messages(messages, max_context_tokens, truncation_strategy)

    tools = skill_schemas if deepseek_enable_tools else None
    final_answer = ""

    # 【修复】max_iterations=0 表示无限制；达到上限时强制模型总结
    iteration = 0
    while True:
        if max_iterations > 0 and iteration >= max_iterations:
            logger.warning("Max iterations reached, forcing final summary")
            messages.append({"role": "user",
                             "content": t("Please summarize what you have learned so far and give your final answer.",
                                          "请基于目前已获得的信息总结并给出最终回答。")})
            if current_backend == "deepseek":
                resp = call_deepseek_nonstream(messages, tools=skill_schemas, timeout=60)
                choice = resp.get("choices", [{}])[0] if isinstance(resp, dict) else {}
                msg = choice.get("message", {}) if isinstance(choice, dict) else {}
                content = msg.get("content", "")
                tool_calls = msg.get("tool_calls", [])
                if not tool_calls:
                    final_answer = content
                    break
            else:
                break
        iteration += 1
        logger.debug(f"Iteration {iteration+1}/{'∞' if max_iterations <= 0 else max_iterations}")

        if show_thinking:
            StreamDisplay.thinking(t(f"Step {iteration+1}/{'∞' if max_iterations <= 0 else max_iterations}", f"第 {iteration+1}/{'∞' if max_iterations <= 0 else max_iterations} 步"))

        # 【0.2.0 统一层】原生直连解析（格式统一：content/reasoning/tool_calls）
        resp = call_deepseek_nonstream(messages, tools)
        if resp.get("error"):
            error_msg = t(f"AI Error: {resp['error']}", f"AI 错误：{resp['error']}")
            logger.error(f"LLM error: {resp['error']}")
            return error_msg
        content = resp.get("content", "") or ""
        tool_calls = resp.get("tool_calls") or []
        reasoning = resp.get("reasoning") if thinking_enabled else None

        # 显示思考链
        if show_thinking and reasoning:
            StreamDisplay.thinking(reasoning)

        if tool_calls:
            # 显示工具调用
            if show_tool_calls:
                for tc in tool_calls:
                    func = tc.get("function", {})
                    name = func.get("name", "unknown")
                    try:
                        args = json.loads(func.get("arguments", "{}"))
                    except:
                        args = {}
                    StreamDisplay.tool_call(name, args)

            messages.append({"role": "assistant", "content": content, "tool_calls": tool_calls})
            tool_results = execute_tool_calls(tool_calls, session_id)

            # 显示工具结果
            if show_tool_results:
                for tr in tool_results:
                    name = "unknown"
                    for tc in tool_calls:
                        if tc.get("id") == tr.get("tool_call_id"):
                            name = tc.get("function", {}).get("name", "unknown")
                            break
                    success = not tr.get("content", "").startswith("Error:")
                    StreamDisplay.tool_result(name, tr.get("content", ""), success)

            for tr in tool_results:
                messages.append(tr)
            continue

        # 最终回复
        if stream_enabled:
            if current_backend == "deepseek":
                stream_gen = call_deepseek_stream(messages, tools=None)
            else:
                stream_gen = call_ollama_stream(messages, tools=None)
            full_response = ""
            for chunk in stream_gen:
                if isinstance(chunk, dict):
                    # 【修复】思考块（reasoning）不拼入最终回复（问题 10 根因）
                    if chunk.get("type") == "thinking":
                        continue
                    # 【0.2.0】统一错误事件
                    if chunk.get("type") == "error":
                        return chunk.get("text", "AI 错误")
                    text = chunk.get("text", "")
                else:
                    text = chunk
                if text.startswith(t("AI Error", "AI 错误")):
                    return text
                full_response += text
            final_answer = full_response
        else:
            final_answer = content

        break

    # 空答案处理（E5 修复）
    if not final_answer or not final_answer.strip():
        logger.warning("react_loop_with_display: Empty final answer")
        if len(messages) > 1:
            last_msg = messages[-1] if messages else {}
            if last_msg.get("role") == "tool" and "Error" in last_msg.get("content", ""):
                final_answer = t(
                    "AI attempted to execute the requested actions but encountered errors. "
                    "Please check the specific error messages above or try rephrasing your request.",
                    "AI尝试执行请求的操作但遇到了错误。请检查上面的具体错误信息，或尝试重新描述您的请求。"
                )
            else:
                final_answer = t(
                    "AI did not generate a valid response. Please try rephrasing your request.",
                    "AI能生成有效回复，请尝试重新描述您的请求。"
                )
        else:
            final_answer = t(
                "AI did not generate a valid response. Please try rephrasing your request.",
                "AI未能生成有效回复，请尝试重新描述您的请求。"
            )

    logger.info(f"react_loop_with_display: returning answer length {len(final_answer)}")
    return final_answer

# =============================================================
# 【修改函数】保持原 react_loop_nonstream 兼容性（调用新函数）
# =============================================================

def react_loop_nonstream(messages: List[Dict], conn=None, max_iterations: int = 0,
                         session_id: str = "default", user_country: str = "UNKNOWN") -> str:
    """
    原非流式 ReAct 循环（保持兼容性，内部调用新函数）
    修改：调用 react_loop_nonstream_with_display，默认显示思考链
    """
    return react_loop_nonstream_with_display(
        messages=messages,
        conn=conn,
        max_iterations=max_iterations,
        session_id=session_id,
        user_country=user_country,
        show_thinking=show_thinking,
        show_tool_calls=True,
        show_tool_results=True
    )

# ========== 【0.2.0】上下文引擎（分层预算 + 70% 预压缩 + 压缩落盘 + 状态外部化） ==========
SESSION_ARCHIVE_DIR = "/LINGOS/data/session_archives"
CONTEXT_PRE_COMPRESS_RATIO = 0.7   # 达预算 70% 预压缩（先生决策：不等超限）
OUTPUT_RESERVE_TOKENS = 4096       # 输出预留


def _context_budget() -> int:
    """上下文预算 = 模型窗口（动态：配置/内置表/URL；无限→用配置） - 输出预留"""
    try:
        cw = llm_unified.get_context_window()
        if cw and cw > 0:
            return max(2048, cw - OUTPUT_RESERVE_TOKENS)
    except Exception:
        pass
    return max(2048, max_context_tokens - OUTPUT_RESERVE_TOKENS)


def _archive_session(session_id: str, messages: List[Dict]) -> str:
    """压缩前旧消息落盘（可回溯——AI 可读，隐私存主机）"""
    try:
        os.makedirs(SESSION_ARCHIVE_DIR, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        fname = os.path.join(SESSION_ARCHIVE_DIR, "%s_%s.json" % (session_id, ts))
        with open(fname, "w", encoding="utf-8") as f:
            json.dump({"session_id": session_id, "ts": datetime.now().isoformat(),
                       "messages": messages}, f, ensure_ascii=False, indent=2)
        return fname
    except Exception as e:
        logger.warning("archive failed: %s", e)
        return ""


def _compress_context(session_id: str, messages: List[Dict],
                      conn=None) -> Tuple[List[Dict], bool, int]:
    """上下文压缩（70% 预算触发）：旧消息分段摘要 + context 事件 + 落盘

    :return: (新消息列表, 是否压缩, 移除消息数)
    """
    budget = _context_budget()
    try:
        total = count_tokens(messages)
    except Exception:
        total = 0
    if total <= budget * CONTEXT_PRE_COMPRESS_RATIO:
        return messages, False, 0

    system_msg = None
    other = []
    for m in messages:
        if m.get("role") == "system":
            system_msg = m
        else:
            other.append(m)
    if not other or len(other) <= 2:
        return messages, False, 0

    # 落盘原始（回溯）
    _archive_session(session_id, other)

    # 分段摘要：最旧一半摘要，保留最近一半原文
    split = max(1, len(other) // 2)
    old_msgs = other[:split]
    keep_msgs = other[split:]
    summary_text = _generate_summary(old_msgs)
    if summary_text:
        summary_content = "[Earlier conversation summary] " + summary_text
    else:
        summary_content = "[Earlier conversation compressed: %d messages]" % len(old_msgs)
    new_other = [{"role": "user", "content": summary_content}] + keep_msgs
    removed = len(old_msgs)

    # context 事件（App 提示条——先生决策）
    try:
        _send_evt(conn, {"type": "context", "action": "summarized",
                         "removed": removed, "tokens": total, "budget": int(budget),
                         "summary": (summary_text or "")[:200]})
        g_events.append({"type": "context", "action": "summarized",
                         "removed": removed, "tokens": total})
    except Exception:
        pass
    logger.info("context compressed: session=%s removed=%d msgs (budget=%d)",
                session_id, removed, int(budget))
    result = ([system_msg] if system_msg else []) + new_other
    return result, True, removed


def _task_pointer_section(session_id: str) -> str:
    """【0.2.0 状态外部化】plan.md/notes.md 路径指针（上下文放指针不放内容——AI-AGENT#4）"""
    try:
        task_dir = "/LINGOS/data/agent_tasks/" + session_id
        plan = os.path.join(task_dir, "plan.md")
        if os.path.exists(plan):
            with open(plan, "r", encoding="utf-8") as f:
                first_line = f.readline().strip()[:120]
            return "## 当前任务\n%s\n任务详情：%s（需要时调用 read_file 读取，上下文只保留指针）" % (first_line, plan)
    except Exception:
        pass
    return ""


def cmd_context_status(session_id: str = "default") -> dict:
    """查询当前会话上下文状态（App 会话页入口——先生决策 F3）"""
    messages = conversations.get(session_id, [])
    try:
        total = count_tokens(messages)
    except Exception:
        total = 0
    budget = _context_budget()
    return {"status": "ok", "data": {
        "session_id": session_id,
        "messages": len(messages),
        "tokens": total,
        "budget": budget,
        "ratio": round(total / budget, 3) if budget else 0,
        "pre_compress_at": CONTEXT_PRE_COMPRESS_RATIO,
        "model": (lambda p: p.model if p else "?")(_current_provider()),
        "provider": (lambda p: p.id if p else "?")(_current_provider()),
    }}


# ========== Unix Socket服务器 ==========
# ========== 【协议v3】会话管理（sessions.json——多端同步） ==========
SESSION_FILE = "/LINGOS/data/sessions.json"

def _sessions_load() -> dict:
    try:
        if os.path.exists(SESSION_FILE):
            with open(SESSION_FILE, encoding="utf-8") as f:
                return json.load(f)
    except Exception:
        pass
    return {}

def _sessions_save(data: dict) -> None:
    try:
        os.makedirs(os.path.dirname(SESSION_FILE), exist_ok=True)
        with open(SESSION_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception as e:
        logger.warning(f"sessions save failed: {e}")

def _session_new_id() -> str:
    import uuid
    return str(uuid.uuid4())[:8]

def cmd_session_list() -> dict:
    data = _sessions_load()
    sessions = []
    for sid, info in data.items():
        sessions.append({"id": sid, "title": info.get("title", "未命名"),
                         "updated": info.get("updated", 0),
                         "message_count": info.get("message_count", 0)})
    sessions.sort(key=lambda x: x.get("updated", 0), reverse=True)
    return {"status": "ok", "data": sessions}

def cmd_session_create(title: str) -> dict:
    data = _sessions_load()
    sid = _session_new_id()
    data[sid] = {"title": title or "新会话", "created": time.time(),
                 "updated": time.time(), "message_count": 0}
    _sessions_save(data)
    return {"status": "ok", "data": {"id": sid, "title": data[sid]["title"]}}

def cmd_session_delete(sid: str) -> dict:
    data = _sessions_load()
    if sid in data:
        del data[sid]
        _sessions_save(data)
        return {"status": "ok"}
    return {"status": "error", "code": "not_found", "msg": "session not found"}

def cmd_session_rename(sid: str, title: str) -> dict:
    data = _sessions_load()
    if sid in data:
        data[sid]["title"] = title or "未命名"
        data[sid]["updated"] = time.time()
        _sessions_save(data)
        return {"status": "ok"}
    return {"status": "error", "code": "not_found", "msg": "session not found"}

def cmd_session_history(sid: str, limit: int = 50) -> dict:
    data = _sessions_load()
    if sid not in data:
        return {"status": "error", "code": "not_found", "msg": "session not found"}
    msgs = data[sid].get("messages", [])
    return {"status": "ok", "data": msgs[-limit:]}

# ========== 【先生决策】App 命令处理（WS command → Python 直通） ==========
import subprocess as _subprocess

def cmd_system_info() -> dict:
    """系统信息（/proc + 磁盘——App 仪表盘真实数据）"""
    try:
        uptime = 0
        with open('/proc/uptime') as f:
            uptime = int(float(f.read().split()[0]))
        mem_total = mem_free = 0
        with open('/proc/meminfo') as f:
            for line in f:
                if line.startswith('MemTotal:'): mem_total = int(line.split()[1]) // 1024
                elif line.startswith('MemFree:'): mem_free = int(line.split()[1]) // 1024
        disk_usage = 0.0
        try:
            st = os.statvfs('/')
            disk_usage = round((1 - st.f_bavail / st.f_bfree) * 100, 2) if st.f_bfree else 0.0
        except Exception: pass
        cpu_usage = 0
        try:
            with open('/proc/stat') as f:
                parts = f.readline().split()[1:]
            total = sum(int(p) for p in parts)
            idle = int(parts[3])
            cpu_usage = round((1 - idle / total) * 100, 1) if total else 0
        except Exception: pass
        return {"status": "ok", "data": {
            "uptime": uptime, "total_ram": mem_total, "free_ram": mem_free,
            "cpu_usage": cpu_usage, "disk_usage": disk_usage,
            "network_rx": 0, "network_tx": 0}}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_file_list(path: str = "/") -> dict:
    try:
        entries = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            is_dir = os.path.isdir(full)
            size = os.path.getsize(full) if not is_dir else 0
            entries.append({"name": name, "type": "dir" if is_dir else "file", "size": size})
        return {"status": "ok", "data": entries}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_file_read(path: str) -> dict:
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return {"status": "ok", "data": f.read(65536)}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_file_write(path: str, content: str = "") -> dict:
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True) if os.path.dirname(path) else None
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        return {"status": "ok"}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_file_delete(path: str) -> dict:
    try:
        os.remove(path) if os.path.isfile(path) else os.rmdir(path)
        return {"status": "ok"}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_ha_search(query: str = "") -> dict:
    """搜索 HA 事件（/LINGOS/HA/YYYY-MM-DD/*.json）"""
    try:
        results = []
        base = "/LINGOS/HA"
        if not os.path.isdir(base):
            return {"status": "ok", "data": []}
        for d in sorted(os.listdir(base), reverse=True)[:7]:
            dp = os.path.join(base, d)
            if not os.path.isdir(dp): continue
            for fn in sorted(os.listdir(dp), reverse=True)[:50]:
                if not fn.endswith('.json'): continue
                try:
                    with open(os.path.join(dp, fn), encoding='utf-8') as f:
                        e = json.load(f)
                    text = json.dumps(e, ensure_ascii=False)
                    if not query or query.lower() in text.lower():
                        results.append({"title": e.get("title") or e.get("type") or fn,
                                        "content": json.dumps(e, ensure_ascii=False)[:200],
                                        "time": e.get("time") or d + " " + fn[4:14]})
                except Exception: continue
                if len(results) >= 50: break
            if len(results) >= 50: break
        return {"status": "ok", "data": results}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_alert_query() -> dict:
    """预警查询（真实记录——/LINGOS/data/alerts/ 分级 L0-L3——先生设计）"""
    try:
        records = []
        base = "/LINGOS/data/alerts"
        if os.path.isdir(base):
            for d in sorted(os.listdir(base), reverse=True)[:14]:  # 最近 14 天
                dp = os.path.join(base, d)
                if not os.path.isdir(dp): continue
                for fn in sorted(os.listdir(dp), reverse=True)[:30]:
                    if not fn.endswith('.json'): continue
                    try:
                        with open(os.path.join(dp, fn), encoding='utf-8') as f:
                            e = json.load(f)
                        records.append({
                            "title": e.get("title") or e.get("type") or "预警",
                            "level": e.get("level", "info"),
                            "content": e.get("content") or e.get("message") or "",
                            "time": e.get("time") or e.get("ts") or (d + " " + fn[4:14]),
                            "source": e.get("source", "alertd"),
                        })
                    except Exception: continue
                    if len(records) >= 50: break
                if len(records) >= 50: break
        # 紧急目录（L0——紧急通道文件）
        urgent_dir = os.path.join(base, "urgent")
        if os.path.isdir(urgent_dir):
            for fn in sorted(os.listdir(urgent_dir), reverse=True)[:10]:
                if not fn.endswith('.json'): continue
                try:
                    with open(os.path.join(urgent_dir, fn), encoding='utf-8') as f:
                        e = json.load(f)
                    records.insert(0, {
                        "title": e.get("title", "紧急"),
                        "level": "L0",
                        "content": e.get("content", ""),
                        "time": e.get("time", fn[4:14]),
                        "source": e.get("source", "urgent"),
                    })
                except Exception: continue
        return {"status": "ok", "data": records}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_memory_search(keyword: str = "") -> dict:
    """记忆搜索（复用 memory_retrieval——降级简单文件搜索）"""
    try:
        try:
            from memory_retrieval import search_memory
            results = search_memory(keyword) if keyword else []
            return {"status": "ok", "data": results if isinstance(results, list) else []}
        except ImportError:
            return {"status": "ok", "data": []}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_memory_write(content: str, mtype: str = "medium") -> dict:
    try:
        from memory_retrieval import write_memory
        write_memory(content, mtype)
        return {"status": "ok"}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_memory_delete(mid: str = "") -> dict:
    try:
        from memory_retrieval import delete_memory
        delete_memory(mid)
        return {"status": "ok"}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

# =============================================================
# 【0.1.9】AI 配置命令族（App 设置页调用）
# =============================================================
TOKEN_USAGE_FILE = "/LINGOS/state/token_usage.jsonl"

def _token_usage_append(provider: str, model: str, prompt_tokens: int, completion_tokens: int) -> None:
    """Token 用量落盘（JSONL 追加——每次模型调用后）"""
    try:
        os.makedirs(os.path.dirname(TOKEN_USAGE_FILE), exist_ok=True)
        rec = {"ts": time.time(), "provider": provider, "model": model,
               "prompt_tokens": prompt_tokens, "completion_tokens": completion_tokens}
        with open(TOKEN_USAGE_FILE, "a") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass

def cmd_token_usage_query(start_time: str = "", end_time: str = "") -> dict:
    """Token 用量查询——{start_time,end_time} 秒级时间戳或空=全部"""
    try:
        records = []
        if os.path.exists(TOKEN_USAGE_FILE):
            with open(TOKEN_USAGE_FILE) as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        r = json.loads(line)
                        records.append(r)
                    except Exception:
                        continue
        # 时间过滤（秒级时间戳——start/end 可选）
        start = float(start_time) if start_time else 0
        end = float(end_time) if end_time else float("inf")
        records = [r for r in records if start <= r.get("ts", 0) <= end]

        total_prompt = sum(r.get("prompt_tokens", 0) for r in records)
        total_completion = sum(r.get("completion_tokens", 0) for r in records)
        # 按模型分组
        by_model = {}
        for r in records:
            m = r.get("model", "unknown")
            by_model.setdefault(m, {"count": 0, "prompt_tokens": 0, "completion_tokens": 0})
            by_model[m]["count"] += 1
            by_model[m]["prompt_tokens"] += r.get("prompt_tokens", 0)
            by_model[m]["completion_tokens"] += r.get("completion_tokens", 0)
        return {"status": "ok", "data": {
            "count": len(records),
            "total_tokens": total_prompt + total_completion,
            "prompt_tokens": total_prompt,
            "completion_tokens": total_completion,
            "by_model": by_model,
            "records": records[-200:],  # 明细最多 200 条
        }}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

# 权限配置（/LINGOS/system/config/ai_permissions.json）
AI_PERM_FILE = "/LINGOS/system/config/ai_permissions.json"
AI_PERMS = ["location", "camera", "record_audio", "record_screen", "accelerometer",
            "phone_state", "installed_apps", "external_storage", "network_control",
            "bluetooth_control", "scan_bluetooth", "launch_app", "install_app",
            "jump_app", "background_data", "background_task", "auto_start"]
AI_PERM_MODES = ["deny", "allow_once", "allow_while", "allow_always", "shadow"]

def _ai_perm_load() -> dict:
    try:
        if os.path.exists(AI_PERM_FILE):
            with open(AI_PERM_FILE) as f:
                return json.load(f)
    except Exception:
        pass
    return {}

def _ai_perm_save(data: dict) -> None:
    try:
        os.makedirs(os.path.dirname(AI_PERM_FILE), exist_ok=True)
        with open(AI_PERM_FILE, "w") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception:
        pass

def cmd_permission_set(perm: str = "", mode: str = "") -> dict:
    """权限模式设置——{perm,mode}"""
    if perm not in AI_PERMS:
        return {"status": "error", "msg": f"未知权限: {perm}（可选: {', '.join(AI_PERMS)}）"}
    if mode not in AI_PERM_MODES:
        return {"status": "error", "msg": f"未知模式: {mode}（可选: {', '.join(AI_PERM_MODES)}）"}
    data = _ai_perm_load()
    data[perm] = mode
    _ai_perm_save(data)
    return {"status": "ok", "data": {perm: mode}}

def cmd_permission_list() -> dict:
    """权限列表 + 当前模式"""
    data = _ai_perm_load()
    return {"status": "ok", "data": {
        "perms": AI_PERMS,
        "modes": AI_PERM_MODES,
        "current": data,
    }}

# 技能配置（/LINGOS/system/config/ai_skills.json——启用≠权限）
AI_SKILL_FILE = "/LINGOS/system/config/ai_skills.json"

def _ai_skill_load() -> dict:
    try:
        if os.path.exists(AI_SKILL_FILE):
            with open(AI_SKILL_FILE) as f:
                return json.load(f)
    except Exception:
        pass
    return {}

def cmd_skill_list_full() -> dict:
    """技能全清单——含风险与启用状态"""
    try:
        from skill_handlers import SKILL_REGISTRY
        skills = []
        for name, info in SKILL_REGISTRY.items():
            skills.append({
                "name": name,
                "risk": info.get("risk", "low"),
                "need_confirm": bool(info.get("need_confirm", False)),
                "description": info.get("description", ""),
            })
    except Exception as e:
        return {"status": "error", "msg": str(e)}
    enabled = _ai_skill_load()
    for s in skills:
        s["enabled"] = enabled.get(s["name"], True)  # 默认启用
    return {"status": "ok", "data": skills}

def cmd_skill_enable(name: str = "", enabled: bool = True) -> dict:
    """技能启用/禁用——{name,enabled}"""
    if not name:
        return {"status": "error", "msg": "缺少技能名"}
    data = _ai_skill_load()
    data[name] = bool(enabled)
    try:
        os.makedirs(os.path.dirname(AI_SKILL_FILE), exist_ok=True)
        with open(AI_SKILL_FILE, "w") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        return {"status": "ok", "data": {name: bool(enabled)}}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

# 人格配置（/LINGOS/system/config/personality.json）
AI_PERSONALITY_FILE = "/LINGOS/system/config/personality.json"
AI_PERSONALITIES = ["nook", "noma"]

def cmd_personality_set(name: str = "") -> dict:
    """人格切换——{name: nook|noma}"""
    if name not in AI_PERSONALITIES:
        return {"status": "error", "msg": f"未知人格: {name}（可选: nook/noma）"}
    try:
        os.makedirs(os.path.dirname(AI_PERSONALITY_FILE), exist_ok=True)
        with open(AI_PERSONALITY_FILE, "w") as f:
            json.dump({"personality": name}, f)
        return {"status": "ok", "data": {"personality": name}}
    except Exception as e:
        return {"status": "error", "msg": str(e)}

def cmd_personality_get() -> dict:
    try:
        if os.path.exists(AI_PERSONALITY_FILE):
            with open(AI_PERSONALITY_FILE) as f:
                return {"status": "ok", "data": json.load(f)}
    except Exception:
        pass
    return {"status": "ok", "data": {"personality": "nook"}}

# AI 提供商配置（/LINGOS/config/ai_config.json——DeepSeek/Ollama）
def cmd_ai_config_set(provider: str = "", api_key: str = "", base_url: str = "",
                      model: str = "") -> dict:
    """AI 提供商配置写入——{provider: deepseek|ollama, api_key, base_url, model}"""
    if provider not in ("deepseek", "ollama"):
        return {"status": "error", "msg": "provider 仅支持 deepseek/ollama"}
    try:
        cfg_path = "/LINGOS/config/ai_config.json"
        cfg = {}
        if os.path.exists(cfg_path):
            with open(cfg_path) as f:
                cfg = json.load(f)
        if provider == "deepseek":
            cfg["backend"] = "deepseek"
            if api_key:
                cfg["deepseek_api_key"] = api_key
            if base_url:
                cfg["deepseek_base_url"] = base_url
            if model:
                cfg["deepseek_model"] = model
        else:
            cfg["backend"] = "ollama"
            if base_url:
                cfg["ollama_url"] = base_url
            if model:
                cfg["ollama_model"] = model
        os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
        with open(cfg_path, "w") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
        return {"status": "ok", "data": {"provider": provider}}
    except Exception as e:
        return {"status": "error", "msg": str(e)}


# =============================================================
# 【0.2.0】模型提供商管理（provider.json——App 同步显示可切换，先生决策）
# =============================================================
def cmd_provider_list() -> dict:
    """模型提供商列表（App 同步显示——主机端配置哪些模型 App 可见）"""
    providers = [p.to_dict() for p in llm_unified.get_providers()]
    active = llm_unified.get_active_provider()
    return {"status": "ok", "data": {"providers": providers,
                                     "active": active.id if active else ""}}


def cmd_model_switch(model_id: str = "") -> dict:
    """切换当前模型（App 调用——先生决策：可切换）"""
    if not model_id:
        return {"status": "error", "msg": "缺少 model_id"}
    ok = llm_unified.set_active_provider(model_id)
    if ok:
        logger.info("model switched to: %s", model_id)
        return {"status": "ok", "data": {"active": model_id}}
    return {"status": "error", "msg": "模型 '%s' 不存在（请先添加提供商）" % model_id}


def cmd_provider_add(pid: str = "", name: str = "", fmt: str = "openai", base_url: str = "",
                     api_key: str = "", model: str = "") -> dict:
    """添加/更新提供商（写 provider.json——App ai_config_set 增强）"""
    if not pid or not base_url or not model:
        return {"status": "error", "msg": "需要 id/base_url/model"}
    providers = llm_unified.get_providers()
    existing = None
    for p in providers:
        if p.id == pid:
            existing = p
            break
    if existing:
        existing.name = name or existing.name
        existing.format = fmt or existing.format
        existing.base_url = base_url
        if api_key:
            existing.api_key = api_key
        existing.model = model
    else:
        providers.append(llm_unified.LLMProvider({
            "id": pid, "name": name or pid, "format": fmt,
            "base_url": base_url, "api_key": api_key, "model": model}))
    llm_unified.save_providers(providers, pid)
    # 同步旧 ai_config.json（deepseek 兼容——保持旧行为不破坏）
    try:
        cfg_path = "/LINGOS/config/ai_config.json"
        cfg = {}
        if os.path.exists(cfg_path):
            with open(cfg_path) as f:
                cfg = json.load(f)
        cfg["backend"] = pid
        if api_key:
            cfg["%s_api_key" % pid] = api_key
        if base_url:
            cfg["%s_base_url" % pid] = base_url
        if model:
            cfg["%s_model" % pid] = model
        with open(cfg_path, "w") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception:
        pass
    return {"status": "ok", "data": {"id": pid, "active": True}}


def cmd_provider_remove(pid: str = "") -> dict:
    """删除提供商（当前活跃被删 → 自动切到第一个）"""
    providers = [p for p in llm_unified.get_providers() if p.id != pid]
    active = llm_unified.get_active_provider()
    new_active = ""
    if active and active.id != pid:
        new_active = active.id
    elif providers:
        new_active = providers[0].id
    llm_unified.save_providers(providers, new_active)
    return {"status": "ok", "data": {"removed": pid, "active": new_active}}

# =============================================================
# 【0.1.9】MCP 管理（额外 MCP——主机以外的工具服务器）
# =============================================================
MCP_FILE = "/LINGOS/state/mcp_servers.json"

def _mcp_load() -> dict:
    try:
        if os.path.exists(MCP_FILE):
            with open(MCP_FILE) as f:
                return json.load(f)
    except Exception:
        pass
    return {}

def _mcp_save(data: dict) -> None:
    try:
        os.makedirs(os.path.dirname(MCP_FILE), exist_ok=True)
        with open(MCP_FILE, "w") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception:
        pass

def cmd_mcp_add(name: str = "", url: str = "", auth_type: str = "none",
                auth_token: str = "") -> dict:
    """添加 MCP 服务器——{name,url,auth_type,auth_token}"""
    if not name or not url:
        return {"status": "error", "msg": "缺少名称或 URL"}
    data = _mcp_load()
    data[name] = {"url": url, "auth_type": auth_type, "auth_token": auth_token,
                  "status": "unknown", "tools": []}
    _mcp_save(data)
    return {"status": "ok", "data": {"name": name, "added": True}}

def cmd_mcp_remove(name: str = "") -> dict:
    data = _mcp_load()
    if name in data:
        del data[name]
        _mcp_save(data)
        return {"status": "ok"}
    return {"status": "error", "msg": "MCP 不存在"}

def cmd_mcp_list() -> dict:
    data = _mcp_load()
    servers = []
    for name, info in data.items():
        servers.append({
            "name": name,
            "url": info.get("url", ""),
            "auth_type": info.get("auth_type", "none"),
            "status": info.get("status", "unknown"),
            "tools": info.get("tools", []),
        })
    return {"status": "ok", "data": {"servers": servers}}

def cmd_mcp_test(name: str = "") -> dict:
    """测试连接（HTTP 探测——超时 5s）"""
    data = _mcp_load()
    if name not in data:
        return {"status": "error", "msg": "MCP 不存在"}
    info = data[name]
    url = info.get("url", "")
    try:
        import urllib.request
        headers = {"User-Agent": "LINGOS-MCP/0.1"}
        if info.get("auth_type") == "bearer" and info.get("auth_token"):
            headers["Authorization"] = "Bearer " + info["auth_token"]
        elif info.get("auth_type") == "api_key" and info.get("auth_token"):
            headers["X-API-Key"] = info["auth_token"]
        req = urllib.request.Request(url.rstrip("/") + "/", headers=headers)
        with urllib.request.urlopen(req, timeout=5) as r:
            info["status"] = "connected" if r.status < 400 else "failed"
            try:
                r.read(2048)
            except Exception:
                pass
    except Exception as e:
        info["status"] = "failed"
        _mcp_save(data)
        return {"status": "error", "msg": "连接失败: " + str(e)[:120]}
    _mcp_save(data)
    return {"status": "ok", "data": {"name": name, "status": info["status"]}}

def _session_append_msg(sid: str, role: str, content: str, max_per_session: int = 200) -> None:
    if not sid or sid == "default":
        return
    try:
        data = _sessions_load()
        if sid not in data:
            return
        msgs = data[sid].setdefault("messages", [])
        msgs.append({"role": role, "content": content[:2000], "ts": time.time()})
        if len(msgs) > max_per_session:
            data[sid]["messages"] = msgs[-max_per_session:]
        data[sid]["message_count"] = len(data[sid]["messages"])
        data[sid]["updated"] = time.time()
        _sessions_save(data)
    except Exception as e:
        logger.warning(f"session append failed: {e}")

def handle_client(conn, addr):
    """
    处理单个客户端连接
    修改：nook_ask 分支调用 react_loop_nonstream_with_display；
          新增 set_log_level 命令处理
    """
    logger.debug(f"handle_client: new connection from {addr}")
    global auto_allow_high_risk
    try:
        data = b""
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        if not data:
            logger.debug("Empty data, closing")
            conn.close()
            return

        req = json.loads(data.decode())
        # 【S2修复】App 参数在 params 嵌套（{type,cmd,params:{...}}）——扁平化到顶层
        _p = req.get("params")
        if isinstance(_p, dict):
            for _k, _v in _p.items():
                req.setdefault(_k, _v)
        cmd = req.get("cmd")
        logger.info(f"Received cmd: {cmd}")

        # ---- 新增：set_log_level 命令 ----
        # 【先生决策】App 命令（WS command → Python 直通）
        if cmd == "system_info":
            conn.send((json.dumps(cmd_system_info(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "file_list":
            conn.send((json.dumps(cmd_file_list(str(req.get("path", "/"))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "file_read":
            conn.send((json.dumps(cmd_file_read(str(req.get("path", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "file_write":
            conn.send((json.dumps(cmd_file_write(str(req.get("path", "")), str(req.get("content", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "file_delete":
            conn.send((json.dumps(cmd_file_delete(str(req.get("path", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "ha_search":
            conn.send((json.dumps(cmd_ha_search(str(req.get("query", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "alert_query":
            conn.send((json.dumps(cmd_alert_query(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "memory_search":
            conn.send((json.dumps(cmd_memory_search(str(req.get("keyword", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "memory_write":
            conn.send((json.dumps(cmd_memory_write(str(req.get("content", "")), str(req.get("type", "medium"))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "memory_delete":
            conn.send((json.dumps(cmd_memory_delete(str(req.get("id", ""))), ensure_ascii=False) + "\n").encode()); return

        # 【协议v3】会话管理
        if cmd == "session_list":
            conn.send((json.dumps(cmd_session_list(), ensure_ascii=False) + "\n").encode())
            return
        if cmd == "session_create":
            conn.send((json.dumps(cmd_session_create(str(req.get("title", "新会话"))), ensure_ascii=False) + "\n").encode())
            return
        if cmd == "session_delete":
            conn.send((json.dumps(cmd_session_delete(str(req.get("id", ""))), ensure_ascii=False) + "\n").encode())
            return
        if cmd == "session_rename":
            conn.send((json.dumps(cmd_session_rename(str(req.get("id", "")), str(req.get("title", ""))), ensure_ascii=False) + "\n").encode())
            return
        if cmd == "session_history":
            conn.send((json.dumps(cmd_session_history(str(req.get("id", "")), int(req.get("limit", 50))), ensure_ascii=False) + "\n").encode())
            return

        # ---- 【0.1.9】AI 配置命令族 ----
        if cmd == "token_usage_query":
            conn.send((json.dumps(cmd_token_usage_query(str(req.get("start_time", "")), str(req.get("end_time", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "permission_set":
            conn.send((json.dumps(cmd_permission_set(str(req.get("perm", "")), str(req.get("mode", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "permission_list":
            conn.send((json.dumps(cmd_permission_list(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "skill_list_full":
            conn.send((json.dumps(cmd_skill_list_full(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "skill_enable":
            conn.send((json.dumps(cmd_skill_enable(str(req.get("name", "")), bool(req.get("enabled", True))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "personality_set":
            conn.send((json.dumps(cmd_personality_set(str(req.get("name", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "personality_get":
            conn.send((json.dumps(cmd_personality_get(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "mcp_add":
            conn.send((json.dumps(cmd_mcp_add(str(req.get("name", "")), str(req.get("url", "")), str(req.get("auth_type", "none")), str(req.get("auth_token", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "mcp_remove":
            conn.send((json.dumps(cmd_mcp_remove(str(req.get("name", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "mcp_list":
            conn.send((json.dumps(cmd_mcp_list(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "mcp_test":
            conn.send((json.dumps(cmd_mcp_test(str(req.get("name", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "ai_config_set":
            conn.send((json.dumps(cmd_ai_config_set(str(req.get("provider", "")), str(req.get("api_key", "")), str(req.get("base_url", "")), str(req.get("model", ""))), ensure_ascii=False) + "\n").encode()); return
        # ---- 【0.2.0】模型提供商（App 同步显示 + 切换） ----
        if cmd == "provider_list":
            conn.send((json.dumps(cmd_provider_list(), ensure_ascii=False) + "\n").encode()); return
        if cmd == "model_switch":
            conn.send((json.dumps(cmd_model_switch(str(req.get("model_id", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "provider_add":
            conn.send((json.dumps(cmd_provider_add(str(req.get("id", "")), str(req.get("name", "")), str(req.get("format", "openai")), str(req.get("base_url", "")), str(req.get("api_key", "")), str(req.get("model", ""))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "provider_remove":
            conn.send((json.dumps(cmd_provider_remove(str(req.get("id", ""))), ensure_ascii=False) + "\n").encode()); return
        # ---- 【0.2.0】上下文状态（App 会话页入口） ----
        if cmd == "context_status":
            conn.send((json.dumps(cmd_context_status(str(req.get("session_id", "default"))), ensure_ascii=False) + "\n").encode()); return
        # ---- 【0.2.0】语音（WS 信令——音频本体走 HTTP REST） ----
        if cmd == "voice_tts":
            ok, path, info = voice_service.tts_synthesize(str(req.get("text", "")), str(req.get("provider", "")), str(req.get("voice", "")), bool(req.get("stream", False)))
            if ok:
                conn.send((json.dumps({"status": "ok", "data": {"file": path, "info": info}}, ensure_ascii=False) + "\n").encode())
            else:
                conn.send((json.dumps({"status": "error", "msg": path}, ensure_ascii=False) + "\n").encode())
            return
        if cmd == "voice_stt":
            ok, text = voice_service.stt_transcribe(str(req.get("file", "")), str(req.get("provider", "")))
            if ok:
                conn.send((json.dumps({"status": "ok", "data": {"text": text}}, ensure_ascii=False) + "\n").encode())
            else:
                conn.send((json.dumps({"status": "error", "msg": text}, ensure_ascii=False) + "\n").encode())
            return
        if cmd == "voice_usage_query":
            conn.send((json.dumps(voice_service.cmd_voice_usage_query(int(req.get("days", 7))), ensure_ascii=False) + "\n").encode()); return
        if cmd == "audio_clear":
            conn.send((json.dumps(voice_service.cmd_audio_clear(), ensure_ascii=False) + "\n").encode()); return

        if cmd == "set_log_level":
            level_str = req.get("level", "info").lower()
            log_level_map = {
                "debug": logging.DEBUG,
                "info": logging.INFO,
                "warn": logging.WARN,
                "warning": logging.WARN,
                "error": logging.ERROR
            }
            level = log_level_map.get(level_str, logging.INFO)
            logger.setLevel(level)
            console_handler.setLevel(level)
            file_handler.setLevel(level)
            logger.info(f"Python log level updated to: {level_str} (level={level})")
            try:
                conn.send(json.dumps({"status": "ok", "content": f"Log level set to {level_str}"}).encode() + b"\n")
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")
            return

        if cmd == "nook_ask":
            global g_events
            g_events = []   # 【批次F】重置事件收集
            prompt = req.get("prompt", "")
            session_id = req.get("session_id", "default")
            timeout = req.get("timeout", socket_timeout)
            show_thinking = req.get("show_thinking", True)
            show_tool_calls = req.get("show_tool_calls", True)
            show_tool_results = req.get("show_tool_results", True)

            if session_id not in conversations:
                conversations[session_id] = []
            if session_id not in user_country_cache:
                user_country_cache[session_id] = "UNKNOWN"
            user_country = user_country_cache[session_id]

            messages = conversations[session_id]
            if not messages:
                gui_mode = bool(req.get("gui_mode", False))
                system_prompt = build_system_prompt("", user_country, session_id, gui_mode=gui_mode)
                messages.append({"role": "system", "content": system_prompt})

            # 【批次1】记忆检索注入（Top-K，独立 system 消息保持前缀缓存稳定；失败不影响主流程）
            try:
                mem_hits = memory_retrieval.retrieve_memories(prompt, memory_top_k)
                mem_section = memory_retrieval.build_memory_section(mem_hits)
                if mem_section:
                    messages.append({"role": "system", "content": mem_section})
                    logger.info("Injected %d memories into session %s", len(mem_hits), session_id)
            except Exception as mem_err:
                logger.warning("Memory retrieval failed: %s", mem_err)

            messages.append({"role": "user", "content": prompt})
            logger.info(f"Processing nook_ask: session={session_id}, prompt='{prompt[:50]}...'")

            try:
                final_answer = react_loop_nonstream_with_display(
                    messages=messages,
                    conn=None,
                    session_id=session_id,
                    user_country=user_country,
                    show_thinking=show_thinking,
                    show_tool_calls=show_tool_calls,
                    show_tool_results=show_tool_results
                )
            except Exception as e:
                logger.error(f"react_loop_nonstream_with_display exception: {traceback.format_exc()}")
                final_answer = t(f"AI processing error: {str(e)}", f"AI处理错误：{str(e)}")

            messages.append({"role": "assistant", "content": final_answer})

            if len(messages) > 50:
                conversations[session_id] = messages[-50:]

            resp = {"status": "ok", "content": final_answer,
                    "events": g_events}   # 【批次F】结构化过程事件

            try:
                conn.send((json.dumps(resp) + "\n").encode())
                logger.info(f"nook_ask response sent (length={len(final_answer)})")
            except BrokenPipeError:
                logger.warning("BrokenPipeError: client disconnected before response sent")
            except Exception as send_err:
                logger.error(f"Send error: {send_err}")

        elif cmd == "nook_ask_stream":
            # 【批次F增强】流式输出：过程事件 + 最终回复逐块实时发送
            # (g_events 已在 nook_ask 分支 global 声明，函数级生效)
            g_events = []
            prompt = req.get("prompt", "")
            session_id = req.get("session_id", "default")
            timeout = req.get("timeout", socket_timeout)
            show_thinking = req.get("show_thinking", True)

            if not prompt:
                conn.send((json.dumps({"type": "done", "content": ""}) + "\n").encode())
                return

            # 【R5b】手动摘要命令（summarize / /summarize）
            if prompt.strip().lower() in ("summarize", "/summarize", "总结对话"):
                if session_id in conversations and conversations[session_id]:
                    summary = generate_summary(conversations[session_id])
                    if summary:
                        conv_msgs = conversations[session_id]
                        conv_msgs[:] = [{"role": "system", "content": "Earlier conversation summary: " + summary}]
                        resp_evt = {"type": "content", "delta": "📋 已生成对话摘要并压缩上下文。\n\n" + summary}
                        conn.send((json.dumps(resp_evt) + "\n").encode())
                        conn.send((json.dumps({"type": "done", "content": summary}) + "\n").encode())
                        return
                    else:
                        conn.send((json.dumps({"type": "content", "delta": "摘要生成失败（AI 不可用）。"}) + "\n").encode())
                else:
                    conn.send((json.dumps({"type": "content", "delta": "当前无对话可摘要。"}) + "\n").encode())
                conn.send((json.dumps({"type": "done", "content": ""}) + "\n").encode())
                return

            if session_id not in conversations:
                conversations[session_id] = []
            if session_id not in user_country_cache:
                user_country_cache[session_id] = "UNKNOWN"
            user_country = user_country_cache[session_id]

            messages = conversations[session_id]
            if not messages:
                gui_mode = bool(req.get("gui_mode", False))
                messages.append({"role": "system", "content": build_system_prompt("", user_country, session_id, gui_mode=gui_mode)})

            # 【0.2.0 双记忆】重要记忆自动注入（importance=high——AI 自决；限预算 ≤800 token）
            # 普通记忆不自动注入（AI 需要时自主 memory_search——协议 AI-AGENT#5）
            try:
                mem_hits = memory_retrieval.retrieve_memories(prompt, memory_top_k, important_only=True)
                mem_section = memory_retrieval.build_memory_section(mem_hits)
                if mem_section and count_tokens([{"role": "system", "content": mem_section}]) <= 800:
                    messages.append({"role": "system", "content": mem_section})
                    logger.info("Injected %d important memories into session %s", len(mem_hits), session_id)
            except Exception as mem_err:
                logger.warning("Memory retrieval failed: %s", mem_err)

            # 【0.2.0 状态外部化】任务指针注入（plan.md 路径——上下文放指针不放内容）
            try:
                task_ptr = _task_pointer_section(session_id)
                if task_ptr:
                    messages.append({"role": "system", "content": task_ptr})
            except Exception:
                pass

            messages.append({"role": "user", "content": prompt})

            # 【0.2.0 上下文引擎】70% 预算预压缩（不等超限）+ context 事件
            try:
                messages, _c1, _r1 = _compress_context(session_id, messages, conn=conn)
            except Exception as e:
                logger.warning("context compress failed: %s", e)

            # 【0.2.0】meta 事件（会话头部——App 显示 model/上下文 token）
            try:
                _prov = _current_provider()
                _cw = llm_unified.get_context_window(_prov)
                _send_evt(conn, {"type": "meta",
                                 "model": _prov.model if _prov else "?",
                                 "provider": _prov.id if _prov else "?",
                                 "max_tokens": _cw or max_context_tokens,
                                 "context_tokens": count_tokens(messages)})
            except Exception:
                pass

            # 【优化2】子 AI 过程事件转发线程（主 AI 对话期间实时显示子 AI 动态）
            stop_flag = threading.Event()
            forwarder = threading.Thread(target=_sub_agent_event_forwarder,
                                         args=(conn, stop_flag), daemon=True)
            forwarder.start()

            try:
                full_answer, usage_info = _react_stream(conn, messages, session_id,
                                                        show_thinking=show_thinking)
            except Exception as e:
                logger.error("react_stream exception: %s", traceback.format_exc())
                full_answer = t(f"AI processing error: {str(e)}", f"AI处理错误：{str(e)}")
                usage_info = {"prompt_tokens": 0, "completion_tokens": 0, "cache_hit": 0}
            finally:
                stop_flag.set()

            messages.append({"role": "assistant", "content": full_answer})
            # 【0.2.0】去掉 50 条硬截断——按 token 预算管理（压缩后继续累积）
            try:
                messages, _c2, _r2 = _compress_context(session_id, messages)
                conversations[session_id] = messages
            except Exception:
                conversations[session_id] = messages

            # 结束事件（完整内容 + 状态行数据：模型/token 用量/缓存命中）
            try:
                if thinking_display == "hidden":
                    conn.send((json.dumps({"type": "thinking_hide"}) + "\n").encode())
                done_evt = {"type": "done", "content": full_answer}
                _prov = _current_provider()
                done_evt["model"] = _prov.model if _prov else (deepseek_model if current_backend == "deepseek" else ollama_model)
                done_evt["provider"] = _prov.id if _prov else current_backend
                done_evt["usage"] = usage_info
                conn.send((json.dumps(done_evt) + "\n").encode())
            except Exception as send_err:
                logger.warning("stream done send failed: %s", send_err)

            # 【0.2.0】Token 用量真实落盘（统一层 usage 提取——修 jsonl 假 0）
            try:
                _prov = _current_provider()
                _token_usage_append(_prov.id if _prov else current_backend,
                                    _prov.model if _prov else "?",
                                    usage_info.get("prompt_tokens", 0),
                                    usage_info.get("completion_tokens", 0))
            except Exception:
                pass
            return

        elif cmd == "agent_view":
            # 【优化2】查看指定子 AI 会话上下文/结果（供 ^N 切换显示）
            orch = agent_orchestrator.get_orchestrator()
            try:
                idx = int(req.get("index", 0))
            except Exception:
                idx = 0
            with orch.lock:
                sessions = list(orch.sessions.values())
            if 0 <= idx < len(sessions):
                s = sessions[idx]
                resp = {
                    "status": "ok",
                    "agent": s.role,
                    "status": s.status,
                    "round": s.round,
                    "max_rounds": s.max_rounds,
                    "result": (s.result or "")[:2000],
                    "dialogue": s.dialogue_log[-10:],
                    "messages": [{"role": m.get("role"), "content": str(m.get("content", ""))[:300]}
                                 for m in s.messages[-6:]]
                }
            else:
                resp = {"status": "error", "message": "invalid index"}
            try:
                conn.send((json.dumps(resp, ensure_ascii=False) + "\n").encode())
            except Exception as send_err:
                logger.warning("agent_view send failed: %s", send_err)
            return

        elif cmd == "summarize":
            # 【批次2】用户手动触发：压缩当前会话上下文（替换历史为摘要）
            session_id = req.get("session_id", "default")
            msgs = conversations.get(session_id, [])
            if len(msgs) <= 1:
                resp = {"status": "ok", "content": t("No conversation to summarize.", "当前无对话可摘要。")}
            else:
                summary_text = _generate_summary(msgs[1:])
                if summary_text:
                    conversations[session_id] = [
                        msgs[0],
                        {"role": "user", "content": "[Conversation summary] " + summary_text}
                    ]
                    resp = {"status": "ok", "content": t("Conversation summarized.", "对话已摘要压缩。")}
                    logger.info("Manual summarize: session %s compressed", session_id)
                else:
                    resp = {"status": "error", "content": t("Summarize failed (AI backend unavailable).",
                                                             "摘要失败（AI 后端不可用）。")}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except Exception as send_err:
                logger.warning("summarize send failed: %s", send_err)
            return

        elif cmd == "agent_msg":
            # 【批次3】子 AI 消息（Hub 路由：子 AI ↔ 主 AI / 子 AI ↔ 子 AI 经主 AI 转发）
            orch = agent_orchestrator.get_orchestrator()
            result, need_main = orch.route(req)
            if need_main and req.get("type") == "ask":
                result["pending_main"] = True
            try:
                conn.send((json.dumps(result) + "\n").encode())
            except Exception as send_err:
                logger.warning("agent_msg send failed: %s", send_err)
            return

        elif cmd == "agent_delegate":
            # 【批次3】主 AI 委派：创建子 AI 会话 + 任务令牌 + 启动执行
            orch = agent_orchestrator.get_orchestrator()
            role = req.get("role", "ai_general")
            prompt = req.get("prompt", "")
            if not prompt:
                conn.send((json.dumps({"status": "error", "message": "missing prompt"}) + "\n").encode())
                return
            task_id, token = orch.create_session(role, prompt[:200])
            orch.run_sub_agent(task_id, prompt)
            resp = {"status": "ok", "task_id": task_id, "auth_token": token,
                    "rounds": orch.max_rounds, "role": role}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except Exception as send_err:
                logger.warning("agent_delegate send failed: %s", send_err)
            return

        elif cmd == "agent_status":
            # 【批次3】查询所有子 AI 任务状态（用户实时显示用）
            orch = agent_orchestrator.get_orchestrator()
            try:
                # 【可配置化】每次查询顺带刷新角色定义（agent_roles.json 热重载）
                agent_orchestrator.reload_roles()
                conn.send((json.dumps({"status": "ok", "agents": orch.session_status_all()}) + "\n").encode())
            except Exception as send_err:
                logger.warning("agent_status send failed: %s", send_err)
            return

        elif cmd == "set_user_name":
            name = req.get("name", "")
            if name and name.strip():
                if save_user_profile(name.strip()):
                    global user_name_cache
                    user_name_cache = name.strip()
                    resp = {"status": "ok", "content": t(f"Name updated to: {name.strip()}", f"称呼已更新为：{name.strip()}")}
                else:
                    resp = {"status": "error", "content": t("Failed to save name", "保存称呼失败")}
            else:
                resp = {"status": "error", "content": t("Name cannot be empty", "称呼不能为空")}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")

        elif cmd == "set_auto_allow":
            value = req.get("value", False)
            auto_allow_high_risk = value
            resp = {"status": "ok", "content": t(f"High-risk auto-auth {'enabled' if value else 'disabled'}", f"高风险自动授权已{'启用' if value else '禁用'}")}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")

        elif cmd == "reload_config":
            try:
                reload_config()
                resp = {"status": "ok", "content": t("Configuration reloaded successfully", "配置已成功重载")}
            except Exception as e:
                logger.error(f"Reload config error: {e}")
                resp = {"status": "error", "content": str(e)}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")

        elif cmd == "get_task_status":
            task_id = req.get("task_id")
            if task_id:
                status = sub_ai_get_status(task_id)
                if status:
                    resp = {"status": "ok", "data": status}
                else:
                    resp = {"status": "error", "content": t("Task not found", "任务不存在")}
            else:
                tasks = sub_ai_get_all_tasks(limit=20)
                resp = {"status": "ok", "data": tasks}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")

        elif cmd == "sub_ai_notification":
            enable = req.get("enable", True)
            sub_ai_set_notification(enable)
            resp = {"status": "ok", "content": t(f"Sub-AI notification {'enabled' if enable else 'disabled'}", f"子AI通知已{'启用' if enable else '禁用'}")}
            try:
                conn.send((json.dumps(resp) + "\n").encode())
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")

        elif cmd == "ping":
            try:
                conn.send(json.dumps({"status": "ok", "content": "pong"}).encode() + b"\n")
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")
        else:
            logger.warning(f"Unknown command: {cmd}")
            try:
                conn.send(json.dumps({"status": "error", "content": t(f"Unknown command: {cmd}", f"未知命令：{cmd}")}).encode() + b"\n")
            except BrokenPipeError:
                logger.debug("BrokenPipeError: client disconnected")

    except json.JSONDecodeError as e:
        logger.error(f"JSON decode error: {e}")
        try:
            conn.send(json.dumps({"status": "error", "content": f"Invalid JSON: {e}"}).encode() + b"\n")
        except:
            pass
    except BrokenPipeError:
        logger.debug("BrokenPipeError: client disconnected (top-level)")
    except Exception as e:
        logger.error(f"Handle client error: {traceback.format_exc()}")
        try:
            conn.send(json.dumps({"status": "error", "content": str(e)}).encode() + b"\n")
        except:
            pass
    finally:
        try:
            conn.close()
        except:
            pass
        logger.debug("Client connection closed")

# ========== 【0.2.0】语音 HTTP REST 端点（8088——音频本体不走 WS，先生决策） ==========
AUDIO_HTTP_PORT = 8088


def _http_auth_ok(token: str) -> bool:
    """Bearer token 校验（读 tokens.json；文件不存在时拒绝——安全默认）"""
    if not token:
        return False
    try:
        tf = "/LINGOS/state/tokens.json"
        if os.path.exists(tf):
            with open(tf, encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, list):
                return any(str(tk) == token for tk in data)
            if isinstance(data, dict):
                tokens = data.get("tokens", [])
                if isinstance(tokens, list):
                    return any(str(tk) == token for tk in tokens)
                return str(data.get("token", "")) == token
    except Exception:
        pass
    return False


from http.server import BaseHTTPRequestHandler


class _VoiceHTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        logger.debug("voice http: " + fmt % args)

    def _check(self) -> bool:
        auth = self.headers.get("Authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else ""
        if not token:
            self.send_response(401)
            self.end_headers()
            self.wfile.write(b'{"error":"missing token"}')
            return False
        if not _http_auth_ok(token):
            self.send_response(401)
            self.end_headers()
            self.wfile.write(b'{"error":"invalid token"}')
            return False
        return True

    def do_GET(self):
        """GET /api/audio/file?name=<文件名>——App 下载合成音频播放（限 audio 目录内）"""
        if self.path.startswith("/api/audio/file"):
            if not self._check():
                return
            from urllib.parse import urlparse, parse_qs
            q = parse_qs(urlparse(self.path).query)
            name = (q.get("name") or [""])[0]
            # 防目录穿越：仅允许文件名（不含路径分隔符）
            if not name or "/" in name or "\\" in name or ".." in name:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b'{"error":"invalid file name"}')
                return
            fp = os.path.join(voice_service.AUDIO_DIR, name)
            if not os.path.exists(fp):
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b'{"error":"file not found"}')
                return
            try:
                self.send_response(200)
                self.send_header("Content-Type", "audio/mpeg")
                self.send_header("Content-Length", str(os.path.getsize(fp)))
                self.end_headers()
                with open(fp, "rb") as f:
                    self.wfile.write(f.read())
            except Exception as e:
                logger.warning("audio get failed: %s", e)
            return
        self.send_response(404)
        self.end_headers()
        self.wfile.write(b'{"error":"not found"}')

    def do_POST(self):
        if self.path.startswith("/api/audio/tts"):
            if not self._check():
                return
            try:
                ln = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(ln).decode("utf-8"))
            except Exception:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b'{"error":"bad json"}')
                return
            ok, path, info = voice_service.tts_synthesize(
                str(body.get("text", "")), str(body.get("provider", "")),
                str(body.get("voice", "")), bool(body.get("stream", False)))
            if ok:
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "audio/mpeg")
                    self.send_header("Content-Length", str(os.path.getsize(path)))
                    self.end_headers()
                    with open(path, "rb") as f:
                        self.wfile.write(f.read())
                except Exception as e:
                    logger.warning("tts http send failed: %s", e)
            else:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": path}, ensure_ascii=False).encode("utf-8"))
        elif self.path.startswith("/api/audio/stt"):
            if not self._check():
                return
            try:
                ctype = self.headers.get("Content-Type", "")
                ln = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(ln)
                boundary = ctype.split("boundary=")[1].strip('"').encode()
                audio_data = b""
                provider = ""
                for part in body.split(b"--" + boundary):
                    if b"filename=" in part and b"\r\n\r\n" in part:
                        audio_data = part.split(b"\r\n\r\n", 1)[1].rsplit(b"\r\n", 1)[0]
                    elif b'name="provider"' in part and b"\r\n\r\n" in part:
                        provider = part.split(b"\r\n\r\n", 1)[1].rsplit(b"\r\n", 1)[0].decode("utf-8", "ignore")
                if not audio_data:
                    self.send_response(400)
                    self.end_headers()
                    self.wfile.write(b'{"error":"no audio file"}')
                    return
                tmp = voice_service._new_audio_name("stt_upload", "wav")
                with open(tmp, "wb") as f:
                    f.write(audio_data)
                ok, text = voice_service.stt_transcribe(tmp, provider)
                self.send_response(200 if ok else 500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"text": text} if ok else {"error": text},
                                            ensure_ascii=False).encode("utf-8"))
            except Exception as e:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}, ensure_ascii=False).encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b'{"error":"not found"}')


def start_voice_http_server():
    try:
        from http.server import HTTPServer
        srv = HTTPServer(("0.0.0.0", AUDIO_HTTP_PORT), _VoiceHTTPHandler)
        logger.info("Voice HTTP server on :%d", AUDIO_HTTP_PORT)
        srv.serve_forever()
    except Exception as e:
        logger.warning("Voice HTTP server failed: %s", e)


def start_unix_socket_server():
    """启动 Unix Socket 服务"""
    logger.info("start_unix_socket_server: Enter")
    if os.path.exists(AI_SOCKET_PATH):
        os.unlink(AI_SOCKET_PATH)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(AI_SOCKET_PATH)
    server.listen(5)
    os.chmod(AI_SOCKET_PATH, 0o666)
    logger.info(f"AI Server listening on {AI_SOCKET_PATH}")
    while True:
        try:
            conn, addr = server.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
        except Exception as e:
            logger.error(f"Accept error: {e}")
            time.sleep(1)

def main():
    logger.info("main: Enter")
    # 启动授权服务
    signal.signal(signal.SIGCHLD, signal.SIG_IGN)
    start_auth_service()

    # 启动授权服务监控
    threading.Thread(target=auth_service_monitor, daemon=True).start()

    # 加载配置
    load_config()
    load_user_profile()
    load_language_map()
    load_skill_help()
    load_knowledge_base()
    load_skill_schemas()

    # ---- 新增：配置摘要日志 ----
    logger.info("main: Configuration summary:")
    logger.info(f"  backend: {current_backend}")
    logger.info(f"  language: {_current_lang}")
    if current_backend == "deepseek":
        logger.info(f"  deepseek model: {deepseek_model}")
        logger.info(f"  deepseek base_url: {deepseek_base_url}")
        if deepseek_api_key:
            logger.info(f"  deepseek api_key: {deepseek_api_key[:8]}... (present)")
        else:
            logger.warning("  deepseek api_key: MISSING")
    else:
        logger.info(f"  ollama url: {ollama_url}")
        logger.info(f"  ollama model: {ollama_model}")
    # ---- 配置摘要日志结束 ----

    # 确保目录存在
    os.makedirs("/LINGOS/system/config", exist_ok=True)
    os.makedirs("/LINGOS/run", exist_ok=True)
    # 【0.2.0】语音服务初始化（清理过期音频）+ HTTP 音频端点（8088——音频本体不走 WS）
    try:
        voice_service.voice_service_init()
        threading.Thread(target=start_voice_http_server, daemon=True).start()
    except Exception as e:
        logger.warning("voice init failed: %s", e)


    # 创建默认 sub_ai.conf
    sub_ai_conf = "/LINGOS/system/config/sub_ai.conf"
    if not os.path.exists(sub_ai_conf):
        with open(sub_ai_conf, "w") as f:
            f.write("max_workers = 0\nresult_wait_timeout = 30\nresult_retry_count = 3\n")

    # 父进程监控
    def monitor_parent():
        while True:
            time.sleep(5)
            try:
                os.kill(PARENT_PID, 0)
            except OSError:
                logger.warning("Parent process terminated, exiting")
                os._exit(0)
            except Exception as e:
                logger.warning(f"Parent monitor error: {e}")
                time.sleep(5)
    threading.Thread(target=monitor_parent, daemon=True).start()

    # 启动服务
    threading.Thread(target=start_unix_socket_server, daemon=True).start()

    logger.info(t("LING OS AI Server started successfully", "LING OS AI 服务器启动成功"))
    while True:
        time.sleep(1)

if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        os._exit(0)
    except Exception as e:
        with open(CRASH_LOG, 'a') as f:
            f.write(f"=== Crash at {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
            f.write(traceback.format_exc())
            f.write("\n")
        logger.critical(f"Unhandled exception: {traceback.format_exc()}")
        os._exit(1)