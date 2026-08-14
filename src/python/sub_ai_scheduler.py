#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Sub-AI Scheduler (All-in-One)
版本: LN-B-5.0.0.0
功能：子AI任务调度与执行（纯线程池），集成原 sub_ai_worker 逻辑
      支持任务状态查询、实时提示控制、详细日志
      修改：日志统一 _T 标准；安全字符串增强
"""

import os
import sys
import json
import threading
import time
import logging
import traceback
import requests
from typing import Dict, List, Optional, Tuple, Any
from datetime import datetime
import concurrent.futures

# ========== 多语言支持 ==========
_current_lang = "en"

def load_language_preference() -> None:
    """从配置文件加载语言偏好"""
    global _current_lang
    try:
        config_path = "/LINGOS/system/config/ai_config.json"
        if os.path.exists(config_path):
            with open(config_path, "r") as f:
                cfg = json.load(f)
            lang = cfg.get("language", "en")
            if lang in ("en", "zh"):
                _current_lang = lang
                logger.info(f"Language preference loaded: {_current_lang}")
    except Exception as e:
        logger.warning(f"Failed to load language preference: {e}")

def t(en: str, zh: str) -> str:
    """返回对应语言的字符串"""
    return zh if _current_lang == "zh" else en

# ========== 日志配置 ==========
LOG_DIR = "/LINGOS/Debug"
os.makedirs(LOG_DIR, exist_ok=True)

log_file = os.path.join(LOG_DIR, "sub_ai_scheduler.log")
log_format = "%(asctime)s [%(levelname)s] [%(name)s] %(message)s"
# 【修复】控制台固定 WARNING（详细进文件）；级别读 ai_config（默认 warning）
_console_h = logging.StreamHandler(sys.stderr)
_console_h.setLevel(logging.WARNING)
_file_h = logging.FileHandler(log_file)
_file_h.setLevel(logging.DEBUG)
logging.basicConfig(
    level=logging.DEBUG,
    format=log_format,
    handlers=[_file_h, _console_h]
)
try:
    with open(CONFIG_PATH if 'CONFIG_PATH' in dir() else "/LINGOS/system/config/ai_config.json", encoding="utf-8") as _cf:
        _lvl = json.load(_cf).get("log_level", "warning")
except Exception:
    _lvl = "warning"
logging.getLogger("SubAIScheduler").setLevel(getattr(logging, _lvl.upper(), logging.WARNING))
logger = logging.getLogger("SubAIScheduler")
logger.info("=== Sub-AI Scheduler starting (all-in-one mode) ===")

# ========== 常量 ==========
SUB_AI_CONF_PATH = "/LINGOS/system/config/sub_ai.conf"
TASKS_STATE_PATH = "/LINGOS/state/tasks.json"
CONFIG_PATH = "/LINGOS/system/config/ai_config.json"
DEFAULT_TIMEOUT = 120

# ========== 技能和系统调用导入 ==========
try:
    from skill_handlers import execute_skill, skill_exists
    from syscall_client import call_syscall
    SKILLS_AVAILABLE = True
    logger.info("Skill handlers and syscall client loaded successfully")
except ImportError as e:
    SKILLS_AVAILABLE = False
    logger.warning(f"Failed to import skill modules: {e}. Skills will be disabled.")

# ========== 配置加载 ==========
def get_api_key_from_config() -> Optional[str]:
    """从配置文件读取有效的 API Key（优先 sub_ai，若无则用主 Key）"""
    try:
        if not os.path.exists(CONFIG_PATH):
            logger.error(f"Config file not found: {CONFIG_PATH}")
            return None
        with open(CONFIG_PATH, "r") as f:
            cfg = json.load(f)
        deepseek_cfg = cfg.get("deepseek", {})
        sub_ai_cfg = cfg.get("sub_ai", {})
        main_key = deepseek_cfg.get("api_key", "").strip()
        sub_key = sub_ai_cfg.get("api_key", "").strip()
        if sub_key.startswith("sk-"):
            return sub_key
        elif main_key.startswith("sk-"):
            return main_key
        else:
            logger.error("No valid API key found")
            return None
    except Exception as e:
        logger.error(f"Failed to read config: {e}")
        return None

def load_deepseek_config() -> Dict:
    """加载 DeepSeek 配置"""
    try:
        with open(CONFIG_PATH, "r") as f:
            cfg = json.load(f)
        deepseek_cfg = cfg.get("deepseek", {})
        return {
            "model": deepseek_cfg.get("model", "deepseek-v4-pro"),
            "base_url": deepseek_cfg.get("base_url", "https://api.deepseek.com"),
            "reasoning_effort": deepseek_cfg.get("reasoning_effort", "high"),
            "enable_tools": deepseek_cfg.get("enable_tools", True),
            "parallel_tools": deepseek_cfg.get("parallel_tools", True),
            "thinking_enabled": cfg.get("thinking_enabled", True)
        }
    except Exception as e:
        logger.warning(f"Failed to load DeepSeek config: {e}, using defaults")
        return {
            "model": "deepseek-v4-pro",
            "base_url": "https://api.deepseek.com",
            "reasoning_effort": "high",
            "enable_tools": True,
            "parallel_tools": True,
            "thinking_enabled": True
        }

# ========== AI API 调用函数 ==========
def call_deepseek_nonstream(messages: List[Dict], tools: Optional[List[Dict]] = None,
                            timeout: int = DEFAULT_TIMEOUT) -> Dict:
    """调用 DeepSeek API（非流式）"""
    logger.debug("call_deepseek_nonstream: Enter, messages_count=%d, tools=%s",
                 len(messages), "provided" if tools else "none")

    key = get_api_key_from_config()
    if not key:
        error_msg = "API key is missing or invalid"
        logger.error(error_msg)
        return {"error": error_msg}

    config = load_deepseek_config()
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    url = f"{config['base_url']}/chat/completions"

    payload = {
        "model": config["model"],
        "messages": messages,
        "stream": False,
        "max_tokens": 4096,
        "temperature": 0.7
    }
    if config["enable_tools"] and tools:
        payload["tools"] = tools
        payload["tool_choice"] = "auto"
    if config["reasoning_effort"] in ("high", "max"):
        payload["reasoning_effort"] = config["reasoning_effort"]

    logger.debug("call_deepseek_nonstream: Request URL=%s, model=%s", url, config["model"])
    try:
        resp = requests.post(url, headers=headers, json=payload, timeout=timeout)
        logger.debug("call_deepseek_nonstream: HTTP status=%d", resp.status_code)
        if resp.status_code == 200:
            return resp.json()
        else:
            error_msg = f"HTTP {resp.status_code}: {resp.text[:500]}"
            logger.error("call_deepseek_nonstream: %s", error_msg)
            return {"error": error_msg}
    except requests.exceptions.Timeout:
        logger.error("call_deepseek_nonstream: Timeout after %ds", timeout)
        return {"error": "Timeout"}
    except Exception as e:
        logger.error("call_deepseek_nonstream: Exception: %s", str(e))
        return {"error": str(e)}

def call_skill(skill_name: str, args: Dict) -> Tuple[bool, str]:
    """执行技能（内部调用）"""
    logger.debug("call_skill: Enter, skill='%s', args=%s", skill_name, args)
    if not SKILLS_AVAILABLE:
        return False, "Skill modules not available"
    if not skill_exists(skill_name):
        return False, f"Skill '{skill_name}' not found"
    args_json = json.dumps(args)
    return execute_skill(skill_name, args_json)

def execute_tool_calls(tool_calls: List[Dict]) -> List[Dict]:
    """执行工具调用列表"""
    logger.debug("execute_tool_calls: Enter, count=%d", len(tool_calls))
    results = []
    for tc in tool_calls:
        func = tc.get("function", {})
        name = func.get("name")
        args_str = func.get("arguments", "{}")
        try:
            args = json.loads(args_str)
        except:
            args = {}
        logger.info("Sub-AI executing tool: %s, args=%s", name, args)
        success, output = call_skill(name, args)
        results.append({
            "tool_call_id": tc.get("id", ""),
            "role": "tool",
            "content": output if success else f"Error: {output}"
        })
        if not success:
            logger.warning("Tool %s failed: %s", name, output)
    return results

def react_loop(prompt: str, max_iterations: int = 10, role: str = "general") -> str:
    """ReAct 循环：执行子AI任务"""
    logger.info("react_loop: Enter, prompt='%s', role='%s'", prompt[:50], role)
    messages = []
    system_prompt = (
        f"You are a sub-AI assistant of LING OS, role: {role}.\n"
        "Your task is to complete the user's request by calling appropriate skills.\n"
        "Use the skills listed below. When done, provide a final answer.\n"
        "Do not mention your internal process; give concise, helpful responses."
    )
    messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": prompt})

    config = load_deepseek_config()
    tools = None

    for iteration in range(max_iterations):
        logger.debug("react_loop: Iteration %d/%d", iteration+1, max_iterations)
        resp = call_deepseek_nonstream(messages, tools)
        if "error" in resp:
            error_msg = f"AI Error: {resp['error']}"
            logger.error("react_loop: %s", error_msg)
            return error_msg

        msg = resp.get("choices", [{}])[0].get("message", {})
        content = msg.get("content", "")
        tool_calls = msg.get("tool_calls", [])

        if tool_calls:
            logger.info("react_loop: Received %d tool calls", len(tool_calls))
            messages.append({"role": "assistant", "content": content, "tool_calls": tool_calls})
            tool_results = execute_tool_calls(tool_calls)
            for tr in tool_results:
                messages.append(tr)
            continue
        else:
            logger.info("react_loop: Final response received")
            return content

    error_msg = "Maximum iterations reached without final response."
    logger.warning("react_loop: %s", error_msg)
    return error_msg

# ========== 任务状态管理 ==========
class TaskStatus:
    PENDING = "pending"
    RUNNING = "running"
    DONE = "done"
    FAILED = "failed"
    CANCELLED = "cancelled"

class TaskInfo:
    """任务信息对象"""
    def __init__(self, task_id: str, prompt: str, role: str = "general"):
        self.task_id = task_id
        self.prompt = prompt
        self.role = role
        self.status = TaskStatus.PENDING
        self.result = None
        self.error = None
        self.created_at = time.time()
        self.updated_at = time.time()
        self.started_at = None
        self.completed_at = None
        self.retry_count = 0
        self.max_retries = 3

    def to_dict(self) -> Dict:
        return {
            "task_id": self.task_id,
            "prompt": self.prompt,
            "role": self.role,
            "status": self.status,
            "result": self.result,
            "error": self.error,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "started_at": self.started_at,
            "completed_at": self.completed_at,
            "retry_count": self.retry_count
        }

    @classmethod
    def from_dict(cls, data: Dict) -> 'TaskInfo':
        task = cls(data["task_id"], data["prompt"], data.get("role", "general"))
        task.status = data.get("status", TaskStatus.PENDING)
        task.result = data.get("result")
        task.error = data.get("error")
        task.created_at = data.get("created_at", time.time())
        task.updated_at = data.get("updated_at", time.time())
        task.started_at = data.get("started_at")
        task.completed_at = data.get("completed_at")
        task.retry_count = data.get("retry_count", 0)
        return task

# ========== 持久化管理 ==========
def load_tasks() -> Dict[str, TaskInfo]:
    """从文件加载任务状态"""
    tasks = {}
    try:
        if not os.path.exists(TASKS_STATE_PATH):
            return tasks
        with open(TASKS_STATE_PATH, "r") as f:
            data = json.load(f)
        for task_data in data.get("tasks", []):
            task = TaskInfo.from_dict(task_data)
            tasks[task.task_id] = task
        logger.info("Loaded %d tasks from %s", len(tasks), TASKS_STATE_PATH)
        return tasks
    except Exception as e:
        logger.error("Failed to load tasks: %s", e)
        return tasks

def save_tasks(tasks: Dict[str, TaskInfo]) -> None:
    """保存任务状态到文件"""
    try:
        os.makedirs(os.path.dirname(TASKS_STATE_PATH), exist_ok=True)
        task_list = [t.to_dict() for t in tasks.values()]
        with open(TASKS_STATE_PATH, "w") as f:
            json.dump({"tasks": task_list}, f, indent=2, ensure_ascii=False)
        logger.debug("Saved %d tasks to %s", len(task_list), TASKS_STATE_PATH)
    except Exception as e:
        logger.error("Failed to save tasks: %s", e)

def cleanup_old_tasks(tasks: Dict[str, TaskInfo], ttl: int = 3600) -> None:
    """清理过期任务"""
    now = time.time()
    to_remove = []
    for task_id, task in tasks.items():
        if task.status in (TaskStatus.DONE, TaskStatus.FAILED, TaskStatus.CANCELLED):
            if now - task.updated_at > ttl:
                to_remove.append(task_id)
        elif task.status == TaskStatus.PENDING:
            if now - task.created_at > 300:
                task.status = TaskStatus.FAILED
                task.error = "Task timed out (orphaned)"
                task.completed_at = now
                task.updated_at = now
                to_remove.append(task_id)
    if to_remove:
        save_tasks(tasks)
        for tid in to_remove:
            tasks.pop(tid, None)
        save_tasks(tasks)
        logger.info("Cleaned up %d old tasks", len(to_remove))

# ========== 子AI调度器 ==========
class SubAIScheduler:
    """子AI调度器（单例）"""
    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        with cls._lock:
            if cls._instance is None:
                cls._instance = super(SubAIScheduler, cls).__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True

        self.max_workers = 5
        self.result_wait_timeout = 30
        self.task_ttl = 3600

        self.tasks: Dict[str, TaskInfo] = {}
        self.task_lock = threading.RLock()
        self.next_id = 0

        self.executor = concurrent.futures.ThreadPoolExecutor(max_workers=self.max_workers)
        logger.info("Thread pool created with max_workers=%d", self.max_workers)

        self.notification_enabled = True

        loaded = load_tasks()
        if loaded:
            cleanup_old_tasks(loaded, self.task_ttl)
            self.tasks = loaded
            for task in self.tasks.values():
                if task.status == TaskStatus.PENDING:
                    logger.info("Resuming pending task: %s", task.task_id)
                    self._submit_task(task)

        self._start_cleanup_thread()

        logger.info("SubAIScheduler initialized (all-in-one mode, notification=%s)",
                    "ON" if self.notification_enabled else "OFF")

    def _start_cleanup_thread(self) -> None:
        def cleanup_loop():
            while True:
                time.sleep(300)
                with self.task_lock:
                    cleanup_old_tasks(self.tasks, self.task_ttl)
                    save_tasks(self.tasks)
        thread = threading.Thread(target=cleanup_loop, daemon=True)
        thread.start()
        logger.debug("Cleanup thread started")

    def _generate_task_id(self) -> str:
        with self.task_lock:
            self.next_id += 1
            return f"subtask_{int(time.time())}_{self.next_id:04d}"

    def _save_task(self, task: TaskInfo) -> None:
        with self.task_lock:
            save_tasks(self.tasks)

    def _submit_task(self, task: TaskInfo) -> None:
        logger.debug("Submitting task: %s", task.task_id)

        def run():
            try:
                task.status = TaskStatus.RUNNING
                task.started_at = time.time()
                task.updated_at = time.time()
                self._save_task(task)
                logger.info("Task %s started (role=%s)", task.task_id, task.role)

                result = react_loop(task.prompt, role=task.role)

                task.completed_at = time.time()
                task.updated_at = time.time()
                if result.startswith("Error:"):
                    task.status = TaskStatus.FAILED
                    task.error = result
                    logger.warning("Task %s failed: %s", task.task_id, result)
                else:
                    task.status = TaskStatus.DONE
                    task.result = result
                    logger.info("Task %s completed successfully", task.task_id)

                if self.notification_enabled:
                    self._notify_status(task)

            except Exception as e:
                task.status = TaskStatus.FAILED
                task.error = str(e)
                task.completed_at = time.time()
                task.updated_at = time.time()
                logger.error("Task %s exception: %s", task.task_id, traceback.format_exc())
            finally:
                self._save_task(task)

        self.executor.submit(run)

    def _notify_status(self, task: TaskInfo) -> None:
        status_map = {
            TaskStatus.DONE: t("Completed", "已完成"),
            TaskStatus.FAILED: t("Failed", "失败"),
        }
        status_text = status_map.get(task.status, task.status)
        msg = t(f"Sub-AI task {task.task_id}: {status_text}",
                f"子AI任务 {task.task_id}: {status_text}")
        logger.info("NOTIFICATION: %s", msg)

    # ========== 公共接口 ==========

    def dispatch_task(self, prompt: str, role: str = "general") -> str:
        logger.info("dispatch_task: prompt='%s', role='%s'", prompt[:50], role)
        task_id = self._generate_task_id()
        task = TaskInfo(task_id, prompt, role)
        with self.task_lock:
            self.tasks[task_id] = task
            save_tasks(self.tasks)
        self._submit_task(task)
        logger.info("Task %s dispatched", task_id)
        return task_id

    def get_task_status(self, task_id: str) -> Optional[Dict]:
        logger.debug("get_task_status: task_id=%s", task_id)
        with self.task_lock:
            task = self.tasks.get(task_id)
            if not task:
                return None
            result = {
                "task_id": task.task_id,
                "status": task.status,
                "role": task.role,
                "created_at": task.created_at,
                "updated_at": task.updated_at,
                "retry_count": task.retry_count
            }
            if task.status == TaskStatus.DONE:
                result["result"] = task.result
            elif task.status == TaskStatus.FAILED:
                result["error"] = task.error
            elif task.status == TaskStatus.RUNNING:
                if task.started_at:
                    result["started_at"] = task.started_at
                    result["elapsed"] = time.time() - task.started_at
            return result

    def get_all_tasks(self, limit: int = 50) -> List[Dict]:
        logger.debug("get_all_tasks: limit=%d", limit)
        with self.task_lock:
            sorted_tasks = sorted(
                self.tasks.values(),
                key=lambda t: t.created_at,
                reverse=True
            )
            return [t.to_dict() for t in sorted_tasks[:limit]]

    def get_statistics(self) -> Dict:
        with self.task_lock:
            stats = {
                "total": len(self.tasks),
                "pending": 0,
                "running": 0,
                "done": 0,
                "failed": 0,
                "cancelled": 0,
                "notification_enabled": self.notification_enabled
            }
            for task in self.tasks.values():
                status = task.status
                if status == TaskStatus.PENDING:
                    stats["pending"] += 1
                elif status == TaskStatus.RUNNING:
                    stats["running"] += 1
                elif status == TaskStatus.DONE:
                    stats["done"] += 1
                elif status == TaskStatus.FAILED:
                    stats["failed"] += 1
                elif status == TaskStatus.CANCELLED:
                    stats["cancelled"] += 1
            return stats

    def set_notification_enabled(self, enabled: bool) -> None:
        self.notification_enabled = enabled
        logger.info("Notification set to %s", "ON" if enabled else "OFF")

    def is_notification_enabled(self) -> bool:
        return self.notification_enabled

    def cancel_task(self, task_id: str) -> bool:
        logger.info("cancel_task: task_id=%s", task_id)
        with self.task_lock:
            task = self.tasks.get(task_id)
            if not task:
                return False
            if task.status == TaskStatus.PENDING:
                task.status = TaskStatus.CANCELLED
                task.updated_at = time.time()
                save_tasks(self.tasks)
                logger.info("Task %s cancelled", task_id)
                return True
            return False

    def shutdown(self) -> None:
        logger.info("Shutting down SubAIScheduler")
        self.executor.shutdown(wait=False)

# ========== 全局单例 ==========
_scheduler: Optional[SubAIScheduler] = None

def get_scheduler() -> SubAIScheduler:
    global _scheduler
    if _scheduler is None:
        load_language_preference()
        _scheduler = SubAIScheduler()
    return _scheduler

# ========== 便捷函数 ==========

def dispatch_task(prompt: str, role: str = "general") -> str:
    return get_scheduler().dispatch_task(prompt, role)

def get_task_status(task_id: str) -> Optional[Dict]:
    return get_scheduler().get_task_status(task_id)

def get_all_tasks(limit: int = 50) -> List[Dict]:
    return get_scheduler().get_all_tasks(limit)

def get_statistics() -> Dict:
    return get_scheduler().get_statistics()

def set_notification_enabled(enabled: bool) -> None:
    get_scheduler().set_notification_enabled(enabled)

def is_notification_enabled() -> bool:
    return get_scheduler().is_notification_enabled()

def cancel_task(task_id: str) -> bool:
    return get_scheduler().cancel_task(task_id)

# ========== 测试入口 ==========
if __name__ == "__main__":
    print("SubAI Scheduler test mode")
    scheduler = get_scheduler()

    task_id = scheduler.dispatch_task("Hello, please introduce yourself")
    print(f"Task dispatched: {task_id}")

    for i in range(20):
        time.sleep(1)
        status = scheduler.get_task_status(task_id)
        if status:
            print(f"Status: {status.get('status')}")
            if status.get('status') == TaskStatus.DONE:
                print(f"Result: {status.get('result', 'N/A')}")
                break
            elif status.get('status') == TaskStatus.FAILED:
                print(f"Error: {status.get('error', 'Unknown')}")
                break

    print("Test complete")
    print(f"Statistics: {scheduler.get_statistics()}")