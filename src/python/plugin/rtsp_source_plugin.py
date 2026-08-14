#!/usr/bin/env python3
"""【0.2.2 插件化示例】RTSP 视频源插件（vision ①连接层扩展点）
展示：现有 rtsp_streamer 包装为插件——visiond 按 camera_source=rtsp 自动发现使用
安装：放入 /LINGOS/plugins/ 目录即可（或随全捆包预装）
"""
import logging
import os
import subprocess
import threading
import time

from lingos_plugin import Plugin

logger = logging.getLogger("RTSPPlugin")


class RtspVideoSourcePlugin(Plugin):
    """RTSP/MJPEG 视频源（基于 rtsp_streamer.py——ffmpeg 拉流）"""
    plugin_type = "video_source"
    source_name = "rtsp"
    name = "rtsp_source"
    version = "0.1"

    def __init__(self):
        self.url = ""
        self.frame_port = 8890
        self.http_port = 8891
        self._proc = None
        self._running = False

    def init(self, config: dict) -> bool:
        self.url = config.get("rtsp_url", "")
        self.frame_port = int(config.get("rtsp_frame_port", 8890))
        self.http_port = int(config.get("rtsp_http_port", 8891))
        if not self.url:
            logger.error("RTSP 插件：未配置 rtsp_url")
            return False
        return True

    def start(self) -> bool:
        """启动 rtsp_streamer（ffmpeg 拉流 → 帧通道 + MJPEG 预览）"""
        py = "/LINGOS/python/bin/python3"
        if not os.path.exists(py):
            py = "python3"
        script = "/LINGOS/bin/rtsp_streamer.py"
        if not os.path.exists(script):
            script = os.path.join(os.path.dirname(__file__), "rtsp_streamer.py")
        cmd = [py, script, "--url", self.url,
               "--frame-port", str(self.frame_port),
               "--http-port", str(self.http_port)]
        try:
            self._proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL)
            self._running = True
            logger.info("RTSP 插件已启动: %s", self.url)
            return True
        except Exception as e:
            logger.error("RTSP 插件启动失败: %s", e)
            return False

    def stop(self) -> None:
        self._running = False
        if self._proc:
            self._proc.terminate()
            self._proc = None

    def get_preview_url(self) -> str:
        return f"http://localhost:{self.http_port}/stream"

    def get_frame_port(self) -> int:
        return self.frame_port


def get_plugin():
    return RtspVideoSourcePlugin()
