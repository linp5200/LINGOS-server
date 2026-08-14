#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS 语音服务（0.2.0——AI-AGENT#8 定稿）
- TTS（合成）+ STT（识别）
- 提供商直连（原生 REST）：elevenlabs/deepgram/azure/minimax/bailian/doubao/mimo（讯飞暂不接入）
- 降级链：提供商失败 → 服务端本地 TTS（espeak-ng → piper）→ 报错（App 落设备本地 TTS）
- 音频落盘 /LINGOS/data/audio/（24h 自动清理 + 手动清空）
- 用量统计 /LINGOS/state/voice_usage.jsonl（字符数/时长/次数）
- 词组机开放：连接到主机的其他服务/设备也可调用（HTTP REST 端点见 ai_server 集成）
"""

import json
import os
import re
import shutil
import subprocess
import time
import logging
import threading
from datetime import datetime, timedelta
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger("VoiceService")

AUDIO_DIR = "/LINGOS/data/audio"
USAGE_FILE = "/LINGOS/state/voice_usage.jsonl"
VOICE_CONFIG_FILE = "/LINGOS/system/config/voice_config.json"
AUDIO_KEEP_HOURS = 24

# 本地 TTS 引擎探测（降级链）
LOCAL_TTS_ENGINES = ["espeak-ng", "espeak", "piper"]

_lock = threading.Lock()


# ========== 配置 ==========
def load_voice_config() -> dict:
    try:
        if os.path.exists(VOICE_CONFIG_FILE):
            with open(VOICE_CONFIG_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
    except Exception as e:
        logger.warning("load_voice_config failed: %s", e)
    return {"providers": [], "active_tts": "", "active_stt": "", "local_tts": "espeak-ng"}


def save_voice_config(cfg: dict) -> bool:
    try:
        os.makedirs(os.path.dirname(VOICE_CONFIG_FILE), exist_ok=True)
        with open(VOICE_CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
        return True
    except Exception as e:
        logger.error("save_voice_config failed: %s", e)
        return False


def get_voice_providers() -> List[dict]:
    return load_voice_config().get("providers", [])


def find_provider(pid: str) -> Optional[dict]:
    for p in get_voice_providers():
        if p.get("id") == pid:
            return p
    return None


# ========== 用量统计 ==========
def _usage_append(provider: str, model: str, chars: int, duration_s: float, op: str) -> None:
    try:
        os.makedirs(os.path.dirname(USAGE_FILE), exist_ok=True)
        rec = {"ts": datetime.now().isoformat(), "provider": provider, "model": model,
               "chars": chars, "duration_s": round(duration_s, 1), "op": op}
        with open(USAGE_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception as e:
        logger.warning("voice usage append failed: %s", e)


def cmd_voice_usage_query(days: int = 7) -> dict:
    """语音用量统计（按提供商聚合）"""
    if not os.path.exists(USAGE_FILE):
        return {"status": "ok", "data": {"total_tts": 0, "total_stt": 0, "by_provider": {}}}
    cutoff = datetime.now() - timedelta(days=max(1, days))
    total_tts = total_stt = 0
    by_provider: Dict[str, dict] = {}
    try:
        with open(USAGE_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                ts = rec.get("ts", "")
                try:
                    if ts and datetime.fromisoformat(ts) < cutoff:
                        continue
                except Exception:
                    pass
                prov = rec.get("provider", "local")
                if prov not in by_provider:
                    by_provider[prov] = {"tts_chars": 0, "stt_seconds": 0, "tts_count": 0, "stt_count": 0}
                if rec.get("op") == "tts":
                    total_tts += 1
                    by_provider[prov]["tts_count"] += 1
                    by_provider[prov]["tts_chars"] += rec.get("chars", 0)
                else:
                    total_stt += 1
                    by_provider[prov]["stt_count"] += 1
                    by_provider[prov]["stt_seconds"] += rec.get("duration_s", 0)
    except Exception as e:
        logger.warning("voice usage query failed: %s", e)
    return {"status": "ok", "data": {"total_tts": total_tts, "total_stt": total_stt,
                                     "by_provider": by_provider}}


# ========== 音频文件管理 ==========
def _ensure_audio_dir() -> None:
    os.makedirs(AUDIO_DIR, exist_ok=True)


def _new_audio_name(prefix: str, ext: str) -> str:
    _ensure_audio_dir()
    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    return os.path.join(AUDIO_DIR, "%s_%s.%s" % (prefix, ts, ext))


def cleanup_audio(hours: int = AUDIO_KEEP_HOURS) -> int:
    """清理超过 N 小时的音频文件（返回删除数）"""
    removed = 0
    try:
        cutoff = time.time() - hours * 3600
        if os.path.isdir(AUDIO_DIR):
            for fn in os.listdir(AUDIO_DIR):
                fp = os.path.join(AUDIO_DIR, fn)
                try:
                    if os.path.isfile(fp) and os.path.getmtime(fp) < cutoff:
                        os.remove(fp)
                        removed += 1
                except Exception:
                    pass
        if removed:
            logger.info("voice cleanup: removed %d audio files (>%dh)", removed, hours)
    except Exception as e:
        logger.warning("cleanup_audio failed: %s", e)
    return removed


def cmd_audio_clear() -> dict:
    """手动清空音频目录"""
    removed = 0
    try:
        if os.path.isdir(AUDIO_DIR):
            for fn in os.listdir(AUDIO_DIR):
                fp = os.path.join(AUDIO_DIR, fn)
                try:
                    if os.path.isfile(fp):
                        os.remove(fp)
                        removed += 1
                except Exception:
                    pass
    except Exception as e:
        return {"status": "error", "message": str(e)}
    return {"status": "ok", "data": {"removed": removed}}


# ========== 本地 TTS（降级链） ==========
def _find_local_engine() -> Optional[str]:
    for eng in LOCAL_TTS_ENGINES:
        if shutil.which(eng):
            return eng
    return None


def _tts_local(text: str, out_path: str) -> bool:
    """本地 TTS：espeak-ng 优先，piper 次之（先生决策：装进安装系统）"""
    eng = _find_local_engine()
    if not eng:
        return False
    try:
        if eng in ("espeak-ng", "espeak"):
            cmd = [eng, "-v", "zh", "-w", out_path, text]
            subprocess.run(cmd, timeout=60, capture_output=True)
            return os.path.exists(out_path) and os.path.getsize(out_path) > 100
        if eng == "piper":
            # piper 需要模型文件；默认尝试 zh_CN-huayan-medium
            model = "/LINGOS/system/piper/zh_CN-huayan-medium.onnx"
            if not os.path.exists(model):
                logger.warning("piper model not found: %s", model)
                return False
            with open(out_path, "wb") as wavf:
                subprocess.run([eng, "-m", model, "-f", text], stdout=wavf,
                               timeout=60, capture_output=True)
            return os.path.exists(out_path) and os.path.getsize(out_path) > 100
    except Exception as e:
        logger.warning("local tts failed: %s", e)
    return False


# ========== TTS 提供商直连（原生 REST——不做转换） ==========
def _tts_elevenlabs(p: dict, text: str, out_path: str) -> bool:
    import requests
    url = "%s/v1/text-to-speech/%s" % (p.get("base_url", "https://api.elevenlabs.io").rstrip("/"),
                                       p.get("voice", "21m00Tcm4TlvDq8ikWAM"))
    r = requests.post(url, headers={"xi-api-key": p.get("api_key", "")},
                      json={"text": text, "model_id": p.get("model", "eleven_multilingual_v2")},
                      timeout=120)
    if r.status_code == 200:
        with open(out_path, "wb") as f:
            f.write(r.content)
        return os.path.getsize(out_path) > 200
    logger.warning("elevenlabs tts %d: %s", r.status_code, r.text[:300])
    return False


def _tts_deepgram(p: dict, text: str, out_path: str) -> bool:
    import requests
    url = "%s/v1/speak?model=%s" % (p.get("base_url", "https://api.deepgram.com").rstrip("/"),
                                    p.get("model", "aura-2-thalia-en"))
    r = requests.post(url, headers={"Authorization": "Token %s" % p.get("api_key", "")},
                      json={"text": text}, timeout=120)
    if r.status_code == 200:
        with open(out_path, "wb") as f:
            f.write(r.content)
        return os.path.getsize(out_path) > 200
    logger.warning("deepgram tts %d: %s", r.status_code, r.text[:300])
    return False


def _tts_azure(p: dict, text: str, out_path: str) -> bool:
    import requests
    base = p.get("base_url", "https://eastasia.tts.speech.microsoft.com").rstrip("/")
    url = "%s/cognitiveservices/v1" % base
    voice = p.get("voice", "zh-CN-XiaoxiaoNeural")
    ssml = ("<speak version='1.0' xml:lang='zh-CN'><voice name='%s'>%s</voice></speak>"
            % (voice, _xml_escape(text)))
    r = requests.post(url, headers={
        "Ocp-Apim-Subscription-Key": p.get("api_key", ""),
        "Content-Type": "application/ssml+xml",
        "X-Microsoft-OutputFormat": "audio-24khz-160kbitrate-mono-mp3",
    }, data=ssml.encode("utf-8"), timeout=120)
    if r.status_code == 200:
        with open(out_path, "wb") as f:
            f.write(r.content)
        return os.path.getsize(out_path) > 200
    logger.warning("azure tts %d: %s", r.status_code, r.text[:300])
    return False


def _tts_minimax(p: dict, text: str, out_path: str) -> bool:
    import requests
    base = p.get("base_url", "https://api.minimax.chat").rstrip("/")
    group_id = p.get("extra", {}).get("group_id", "")
    url = "%s/v1/t2a_v2?GroupId=%s" % (base, group_id)
    r = requests.post(url, headers={"Authorization": "Bearer %s" % p.get("api_key", "")},
                      json={"text": text, "model": p.get("model", "speech-02-hd"),
                            "voice_setting": {"voice_id": p.get("voice", "female-shaonv")}},
                      timeout=120)
    if r.status_code == 200:
        data = r.json()
        audio = data.get("data", {}).get("audio")
        if audio:
            import base64
            with open(out_path, "wb") as f:
                f.write(base64.b64decode(audio))
            return os.path.getsize(out_path) > 200
    logger.warning("minimax tts %d: %s", r.status_code, r.text[:300])
    return False


def _tts_bailian(p: dict, text: str, out_path: str) -> bool:
    import requests
    url = "%s/api/v1/services/tts/text-to-speech" % p.get("base_url",
                                                          "https://dashscope.aliyuncs.com").rstrip("/")
    r = requests.post(url, headers={"Authorization": "Bearer %s" % p.get("api_key", "")},
                      json={"model": p.get("model", "sambert-zhichu-v1"),
                            "input": {"text": text},
                            "parameters": {"format": "mp3", "sample_rate": 24000}},
                      timeout=120)
    if r.status_code == 200:
        data = r.json()
        url2 = data.get("output", {}).get("audio")
        if url2:
            rr = requests.get(url2, timeout=120)
            with open(out_path, "wb") as f:
                f.write(rr.content)
            return os.path.getsize(out_path) > 200
    logger.warning("bailian tts %d: %s", r.status_code, r.text[:300])
    return False


def _tts_doubao(p: dict, text: str, out_path: str) -> bool:
    import requests
    url = "%s/api/v1/tts" % p.get("base_url", "https://ark.cn-beijing.volces.com").rstrip("/")
    r = requests.post(url, headers={"Authorization": "Bearer %s" % p.get("api_key", "")},
                      json={"text": text, "voice_type": p.get("voice", "zh_female_wanwanxiaohe_mars_bigtts"),
                            "response_format": "mp3"},
                      timeout=120)
    if r.status_code == 200:
        data = r.json()
        audio = data.get("data", [{}])[0].get("audio")
        if audio:
            import base64
            with open(out_path, "wb") as f:
                f.write(base64.b64decode(audio))
            return os.path.getsize(out_path) > 200
    logger.warning("doubao tts %d: %s", r.status_code, r.text[:300])
    return False


def _tts_mimo(p: dict, text: str, out_path: str) -> bool:
    import requests
    url = "%s/api/tts" % p.get("base_url", "https://api.xiaomi.com").rstrip("/")
    r = requests.post(url, headers={"Authorization": "Bearer %s" % p.get("api_key", "")},
                      json={"text": text, "voice": p.get("voice", "xiaomi")}, timeout=120)
    if r.status_code == 200:
        with open(out_path, "wb") as f:
            f.write(r.content)
        return os.path.getsize(out_path) > 200
    logger.warning("mimo tts %d: %s", r.status_code, r.text[:300])
    return False


_TTS_HANDLERS = {
    "elevenlabs": _tts_elevenlabs, "deepgram": _tts_deepgram, "azure_tts": _tts_azure,
    "minimax": _tts_minimax, "bailian": _tts_bailian, "doubao": _tts_doubao,
    "mimo": _tts_mimo,
}


def _xml_escape(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def tts_synthesize(text: str, provider_id: str = "", voice: str = "",
                   stream: bool = False) -> Tuple[bool, str, dict]:
    """TTS 合成 → (success, file_path 或错误, info)

    降级链：指定提供商 → 本地 TTS（espeak-ng/piper）→ 失败（App 落设备本地 TTS）
    """
    text = (text or "").strip()
    if not text:
        return False, "文本为空", {}
    cfg = load_voice_config()
    pid = provider_id or cfg.get("active_tts", "")
    used_provider = "local"
    used_model = ""
    duration_est = max(1.0, len(text) / 4.0)  # 估算：约 4 字/秒

    out_path = _new_audio_name("tts", "mp3")

    if pid:
        p = find_provider(pid)
        if not p:
            return False, "语音提供商 '%s' 未配置" % pid, {}
        handler = _TTS_HANDLERS.get(p.get("id"))
        if handler:
            try:
                ok = handler(p, text, out_path)
                if ok:
                    used_provider = p.get("id")
                    used_model = p.get("model", "")
                    _usage_append(used_provider, used_model, len(text), duration_est, "tts")
                    return True, out_path, {"provider": used_provider, "model": used_model,
                                            "chars": len(text), "duration_s": duration_est}
                logger.warning("provider tts failed (%s), falling back to local", p.get("id"))
            except Exception as e:
                logger.warning("provider tts exception (%s): %s, falling back to local", p.get("id"), e)
        else:
            return False, "语音提供商 '%s' 暂不支持 TTS" % pid, {}

    # 本地降级
    out_wav = _new_audio_name("tts_local", "wav")
    if _tts_local(text, out_wav):
        used_provider = "local"
        used_model = _find_local_engine() or "espeak-ng"
        _usage_append("local", used_model, len(text), duration_est, "tts")
        return True, out_wav, {"provider": "local", "model": used_model,
                               "chars": len(text), "duration_s": duration_est}

    # 清理可能残留的失败文件
    for fp in (out_path, out_wav):
        try:
            if os.path.exists(fp):
                os.remove(fp)
        except Exception:
            pass

    return False, "语音合成失败：未配置提供商且服务端无本地 TTS 引擎（espeak-ng/piper）——App 将使用设备本地 TTS", {}


# ========== STT（识别） ==========
def _stt_deepgram(p: dict, audio_path: str) -> Tuple[bool, str]:
    import requests
    url = "%s/v1/listen" % p.get("base_url", "https://api.deepgram.com").rstrip("/")
    with open(audio_path, "rb") as f:
        r = requests.post(url, headers={"Authorization": "Token %s" % p.get("api_key", "")},
                          params={"model": p.get("model", "nova-3"), "language": "zh"},
                          data=f, timeout=180)
    if r.status_code == 200:
        data = r.json()
        txt = (data.get("results", {}).get("channels", [{}])[0]
               .get("alternatives", [{}])[0].get("transcript", ""))
        return True, txt
    logger.warning("deepgram stt %d: %s", r.status_code, r.text[:300])
    return False, "Deepgram 识别失败（HTTP %d）" % r.status_code


def _stt_bailian(p: dict, audio_path: str) -> Tuple[bool, str]:
    import requests
    url = "%s/api/v1/services/asr/transcription" % p.get("base_url",
                                                         "https://dashscope.aliyuncs.com").rstrip("/")
    with open(audio_path, "rb") as f:
        r = requests.post(url, headers={"Authorization": "Bearer %s" % p.get("api_key", "")},
                          params={"model": p.get("model", "paraformer-realtime-v2")},
                          data=f, timeout=180)
    if r.status_code == 200:
        data = r.json()
        txt = (data.get("output", {}).get("sentence", [{}])[0].get("text", "")
               if data.get("output") else "")
        return True, txt
    logger.warning("bailian stt %d: %s", r.status_code, r.text[:300])
    return False, "百炼识别失败（HTTP %d）" % r.status_code


def _stt_doubao(p: dict, audio_path: str) -> Tuple[bool, str]:
    import requests
    url = "%s/api/v1/asr" % p.get("base_url", "https://ark.cn-beijing.volces.com").rstrip("/")
    with open(audio_path, "rb") as f:
        r = requests.post(url, headers={"Authorization": "Bearer %s" % p.get("api_key", "")},
                          data=f, timeout=180)
    if r.status_code == 200:
        data = r.json()
        txt = (data.get("result", {}) or {}).get("text", "")
        return True, txt
    logger.warning("doubao stt %d: %s", r.status_code, r.text[:300])
    return False, "火山识别失败（HTTP %d）" % r.status_code


_STT_HANDLERS = {
    "deepgram": _stt_deepgram, "bailian": _stt_bailian, "doubao": _stt_doubao,
}


def stt_transcribe(audio_path: str, provider_id: str = "") -> Tuple[bool, str]:
    """STT 识别 → (success, text)"""
    if not audio_path or not os.path.exists(audio_path):
        return False, "音频文件不存在：%s" % audio_path
    cfg = load_voice_config()
    pid = provider_id or cfg.get("active_stt", "")
    if not pid:
        return False, "未配置 STT 提供商（服务端代理模式需要）"
    p = find_provider(pid)
    if not p:
        return False, "语音提供商 '%s' 未配置" % pid
    handler = _STT_HANDLERS.get(p.get("id"))
    if not handler:
        return False, "语音提供商 '%s' 暂不支持 STT" % pid
    ok, txt = handler(p, audio_path)
    if ok:
        dur = max(1.0, os.path.getsize(audio_path) / 32000.0)
        _usage_append(p.get("id"), p.get("model", ""), len(txt), dur, "stt")
    return ok, txt


# ========== 服务端启动时清理 ==========
def voice_service_init() -> None:
    """启动时清理过期音频 + 确保目录"""
    _ensure_audio_dir()
    cleanup_audio(AUDIO_KEEP_HOURS)
    logger.info("voice_service initialized (audio dir: %s)", AUDIO_DIR)
