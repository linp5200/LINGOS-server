# LING OS 插件化架构（0.2.2 长期主线——先生裁决 2026-08-14）

> 一切功能以插件形式增减（减少或增加某些功能）——后续为长期开发主线
> 本批预留扩展点 + 示例插件

## 一、现有插件系统（骨架已有）

```
src/python/plugin/
├── lingos_plugin.py     # 插件基类 + @Skill/@Command 装饰器
├── plugin_discovery.py  # 插件发现（扫描目录）+ 目录确保
└── plugin_loader.py     # 加载/卸载/技能执行/命令执行（单例）
```

- 插件目录：`/LINGOS/plugins/`（用户可增减）
- 加载：`PluginLoader.load_all()` → discover → import → 注册 Skill/Command

## 二、本批新增扩展点（vision 功能链插件化）

| 扩展点 | 插件类型 | 示例 | 归属 |
|---|---|---|---|
| **视频源** | `VideoSourcePlugin` | v4l2 / rtsp_streamer / mjpeg | vision ①连接层 |
| **检测引擎** | `DetectionPlugin` | yolo_service / 自研模型 | vision ②检测层 |
| **OCR 引擎** | `OcrEnginePlugin` | PaddleOCR / Tesseract（双引擎插件化） | vision ②检测层 |
| **标定算法** | `CalibrationPlugin` | planar（自动）/ 3d（二维码） | vision ③空间层 |
| **呈现端** | `PresenterPlugin` | App / Web / TUI | vision ⑤呈现层 |
| **AI 路径** | `VisionAiPlugin` | 文本化 / 多模态 | vision ⑥AI层 |

## 三、插件基类扩展（vision 插件示例）

```python
# 示例：视频源插件（vision ①连接层）
from lingos_plugin import Plugin

class VideoSourcePlugin(Plugin):
    """视频源插件基类——新采集后端（RTSP/MJPEG/CSI）实现此接口"""
    plugin_type = "video_source"
    source_name = ""          # v4l2 / rtsp / mjpeg / csi
    def init(self, config: dict) -> bool: ...
    def capture(self) -> bytes: ...        # 返回一帧 JPEG
    def get_preview_url(self) -> str: ...  # 预览地址
    def cleanup(self) -> None: ...

# 示例：OCR 引擎插件（vision ②检测层——双引擎皆插件化）
class OcrEnginePlugin(Plugin):
    plugin_type = "ocr_engine"
    engine_name = ""          # paddle / tesseract
    def init(self) -> bool: ...
    def recognize(self, image: bytes) -> list: ...  # [{text, confidence, box}]

# 示例：呈现端插件（vision ⑤呈现层——凡支持播放的都可以）
class PresenterPlugin(Plugin):
    plugin_type = "presenter"
    def render_frame(self, frame: bytes, detections: list) -> None: ...
```

## 四、visiond 插件化接入流程

```
visiond 启动 → PluginLoader.load_all()
  → discover video_source 插件（按 vision.conf 的 camera_source 匹配）
  → discover ocr_engine 插件（PaddleOCR + Tesseract 都注册——双引擎）
  → discover presenter 插件（App/Web 都注册）
运行 → 采集(capture) → 检测(yolo) → OCR(recognize) → 呈现(render)
       → 事件上报 ai_server → App 广播（vision_event）
```

## 五、用户增减功能

```bash
# 增加功能：放入插件目录
cp my_camera_plugin.py /LINGOS/plugins/
# 减少功能：删除/禁用对应插件
rm /LINGOS/plugins/some_plugin.py
# 或配置禁用（插件 manifest 支持 enabled 字段）
```

## 六、后续路线

1. 本批：扩展点定义 + 现有组件插件化包装（rtsp_streamer/ocr_service/calibration_service 提供插件适配）
2. 后续：插件市场/远程安装/版本管理（与系统 update 联动）
3. 简略版/压缩版（体积敏感）与完整版同源——插件裁剪出
