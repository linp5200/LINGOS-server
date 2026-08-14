#!/usr/bin/env python3
"""【0.2.2 vision】AI 视觉调用（先生裁决 2026-08-14 双路径）
路径 A（模型无视觉）：摄像头识别转文本（YOLO 检测结果 + OCR 文字）→ 文本喂 LLM
路径 B（模型多模态）：直接截图/视频帧 base64 → 喂多模态 LLM
依赖：llm_unified（统一调用层——多模态 image_url 支持已加）
"""
import base64
import json
import logging
import os

logging.basicConfig(level=logging.INFO, format="%(asctime)s [VISION_AI] %(message)s")
log = logging.getLogger("VISION_AI")

import llm_unified


def _provider_supports_vision(provider) -> bool:
    """多模态能力判断：provider 配置或模型名含视觉标识"""
    if not provider:
        return False
    extra = getattr(provider, "extra", None) or {}
    if extra.get("vision"):
        return True
    model = (getattr(provider, "model", "") or "").lower()
    return any(k in model for k in ["gpt-4o", "gpt-4.1", "gemini", "claude-3", "claude-4", "qwen-vl", "vision", "glm-4v"])


# ============================================================
# 路径 A：识别转文本（无视觉模型）
# ============================================================

def build_vision_text(detections: list, ocr_results: list) -> str:
    """YOLO 检测 + OCR 结果 → 文本化描述（喂无视觉 LLM）"""
    parts = []
    if detections:
        parts.append("画面中检测到以下物体：")
        for d in detections:
            label = d.get("label", "未知")
            conf = d.get("confidence", 0)
            wx = d.get("world_x")
            wy = d.get("world_y")
            loc = f"（位置: x={wx:.0f}cm, y={wy:.0f}cm）" if wx is not None else ""
            parts.append(f"- {label}（置信度 {conf:.0%}）{loc}")
    if ocr_results:
        parts.append("画面中识别到文字：")
        for o in ocr_results:
            parts.append(f"- “{o.get('text', '')}”")
    if not parts:
        parts.append("画面中未检测到明显物体或文字。")
    return "\n".join(parts)


def call_with_detections(question: str, detections: list, ocr_results: list,
                         session_id: str = "vision") -> str:
    """路径 A：识别结果文本化 → LLM 理解（先生裁决：无视觉模型走此路）"""
    vision_text = build_vision_text(detections, ocr_results)
    messages = [
        {"role": "system", "content": "你是 LING OS 视觉助手。用户提供摄像头画面识别结果，"
                                      "请基于这些信息回答用户的问题。不知道就说明不知道。"},
        {"role": "user", "content": f"摄像头识别结果：\n{vision_text}\n\n问题：{question}"},
    ]
    try:
        provider = llm_unified.get_active_provider()
        resp = llm_unified.call_llm_nonstream(messages, tools=None)
        return resp.get("content", "") if resp else "（AI 调用无返回）"
    except Exception as e:
        log.error("路径A调用失败: %s", str(e)[:150])
        return f"（视觉分析失败：{str(e)[:100]}）"


# ============================================================
# 路径 B：多模态直喂（截图/视频帧）
# ============================================================

def _image_to_base64(image_path: str) -> str:
    with open(image_path, "rb") as f:
        return base64.b64encode(f.read()).decode()


def call_with_image(question: str, image_path: str, session_id: str = "vision") -> str:
    """路径 B：截图/视频帧 → base64 → 多模态 LLM（先生裁决：多模态模型走此路）
    image_path：本地图片路径（截图/抓帧）"""
    provider = llm_unified.get_active_provider()
    if not _provider_supports_vision(provider):
        log.info("当前模型无多模态——路径A兜底需先识别（call_with_detections）")
        return {"error": "provider_no_vision", "msg": "当前模型不支持图片——请走识别转文本路径"}
    b64 = _image_to_base64(image_path)
    data_url = f"data:image/jpeg;base64,{b64}"
    messages = [
        {"role": "system", "content": "你是 LING OS 视觉助手。请基于用户提供的图片回答。"},
        {"role": "user", "content": [
            {"type": "text", "text": question},
            {"type": "image_url", "image_url": {"url": data_url}},
        ]},
    ]
    try:
        resp = llm_unified.call_llm_nonstream(messages, tools=None)
        return resp.get("content", "") if resp else "（AI 调用无返回）"
    except Exception as e:
        log.error("路径B调用失败: %s", str(e)[:150])
        return f"（视觉分析失败：{str(e)[:100]}）"


def vision_ask(question: str, image_path: str = "", detections: list = None,
               ocr_results: list = None) -> dict:
    """统一入口：有图+多模态 → 路径B；无多模态或仅识别结果 → 路径A"""
    provider = llm_unified.get_active_provider()
    if image_path and _provider_supports_vision(provider):
        result = call_with_image(question, image_path)
        if not (isinstance(result, dict) and result.get("error")):
            return {"path": "B_multimodal", "answer": result}
    return {"path": "A_text", "answer": call_with_detections(question, detections or [], ocr_results or [])}
