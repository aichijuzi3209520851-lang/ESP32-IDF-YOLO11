"""
PC 端推理服务
============
从 ESP32 拉取原始 MJPEG 视频流，用 YOLO11s 5 类模型做推理，
然后输出带标注的 MJPEG 流 + JSON 状态 API。

使用方法:
    python pc_inference_server.py --esp32-host 192.168.1.100 --model pc_5class_model.onnx

然后浏览器打开 dashboard.html，切换到 "PC 边缘模式" 即可看到 5 类检测结果。
"""

import argparse
import time
import threading
import queue
from pathlib import Path

import cv2
import numpy as np
from flask import Flask, Response, jsonify, render_template_string
from ultralytics import YOLO

SCRIPT_DIR = Path(__file__).resolve().parent

# 5 类颜色（和 ESP32 端区分开，用更丰富的颜色）
CLASS_COLORS = {
    0: (255, 80, 80),    # bird_drop - 红
    1: (80, 180, 255),   # dust - 蓝
    2: (255, 165, 0),    # electrical_damage - 橙
    3: (180, 50, 200),   # physical_damage - 紫
    4: (200, 200, 255),  # snow_covered - 浅蓝
}
CLASS_NAMES = ["bird_drop", "dust", "electrical_damage", "physical_damage", "snow_covered"]


class PCInferenceServer:
    def __init__(self, esp32_host: str, model_path: str, port: int = 5000,
                 conf_threshold: float = 0.25, iou_threshold: float = 0.45):
        self.esp32_host = esp32_host
        self.port = port
        self.conf_threshold = conf_threshold
        self.iou_threshold = iou_threshold

        # 帧队列：拉流线程 → 推理线程
        self.frame_queue = queue.Queue(maxsize=2)
        # 最新结果
        self.latest_frame = None
        self.latest_detections = []
        self.latest_latency_ms = 0
        self.latest_fps = 0.0
        self.frame_count = 0
        self.running = True

        # 加载模型
        print(f"加载模型: {model_path}")
        self.model = YOLO(model_path)
        print(f"模型加载完成: {CLASS_NAMES}")

        # Flask 应用
        self.app = Flask(__name__)
        self._setup_routes()

    def _setup_routes(self):
        @self.app.route('/api/health')
        def health():
            return jsonify({
                "status": "ok",
                "model": "YOLO11s",
                "classes": CLASS_NAMES,
                "input_size": 640,
                "esp32_host": self.esp32_host,
            })

        @self.app.route('/api/inference')
        def inference_status():
            return jsonify({
                "ready": self.latest_frame is not None,
                "latency_ms": round(self.latest_latency_ms, 1),
                "fps": round(self.latest_fps, 2),
                "detections": self.latest_detections,
                "model": "pc_yolo11s",
                "classes": 5,
                "class_names": CLASS_NAMES,
            })

        @self.app.route('/annotated-stream')
        def annotated_stream():
            return Response(
                self._generate_mjpeg(),
                mimetype='multipart/x-mixed-replace; boundary=frame',
            )

    def _generate_mjpeg(self):
        """生成 MJPEG 流"""
        boundary = b'--frame\r\n'
        while self.running:
            if self.latest_frame is None:
                time.sleep(0.01)
                continue

            _, jpeg = cv2.imencode('.jpg', self.latest_frame, [cv2.IMWRITE_JPEG_QUALITY, 70])
            jpeg_bytes = jpeg.tobytes()

            yield (
                boundary
                + b'Content-Type: image/jpeg\r\n'
                + f'Content-Length: {len(jpeg_bytes)}\r\n\r\n'.encode()
                + jpeg_bytes
                + b'\r\n'
            )

    def _stream_reader_thread(self):
        """从 ESP32 拉取 MJPEG 流"""
        stream_url = f"http://{self.esp32_host}:81/stream"
        print(f"连接 ESP32 原始流: {stream_url}")

        import urllib.request

        while self.running:
            try:
                stream = urllib.request.urlopen(stream_url, timeout=5)
                bytes_data = b''
                while self.running:
                    chunk = stream.read(4096)
                    if not chunk:
                        break
                    bytes_data += chunk

                    # 查找 JPEG 帧边界
                    start = bytes_data.find(b'\xff\xd8')
                    end = bytes_data.find(b'\xff\xd9')
                    if start != -1 and end != -1 and end > start:
                        jpg = bytes_data[start:end + 2]
                        bytes_data = bytes_data[end + 2:]

                        # 解码
                        frame = cv2.imdecode(
                            np.frombuffer(jpg, dtype=np.uint8),
                            cv2.IMREAD_COLOR
                        )
                        if frame is not None:
                            # 队列满了就丢旧帧，保持最新
                            if self.frame_queue.full():
                                try:
                                    self.frame_queue.get_nowait()
                                except queue.Empty:
                                    pass
                            self.frame_queue.put(frame)

            except Exception as e:
                print(f"拉流断开，3秒后重连: {e}")
                time.sleep(3)

    def _inference_thread(self):
        """推理线程：从队列取帧 → 推理 → 画框 → 更新最新结果"""
        last_fps_time = time.time()
        fps_frame_count = 0

        while self.running:
            try:
                frame = self.frame_queue.get(timeout=1)
            except queue.Empty:
                continue

            t0 = time.time()

            # YOLO 推理
            results = self.model(
                frame,
                conf=self.conf_threshold,
                iou=self.iou_threshold,
                verbose=False,
            )

            t1 = time.time()
            self.latest_latency_ms = (t1 - t0) * 1000

            # 画框 + 收集检测结果
            detections = []
            annotated = frame.copy()

            for result in results:
                boxes = result.boxes
                if boxes is None:
                    continue

                for box in boxes:
                    cls_id = int(box.cls[0])
                    conf = float(box.conf[0])
                    x1, y1, x2, y2 = box.xyxy[0].cpu().numpy().astype(int)

                    detections.append({
                        "class": cls_id,
                        "class_name": CLASS_NAMES[cls_id],
                        "confidence": round(conf, 3),
                        "x1": int(x1), "y1": int(y1),
                        "x2": int(x2), "y2": int(y2),
                    })

                    # 画框
                    color = CLASS_COLORS.get(cls_id, (0, 255, 0))
                    cv2.rectangle(annotated, (x1, y1), (x2, y2), color, 2)

                    # 标签
                    label = f"{CLASS_NAMES[cls_id]} {conf:.2f}"
                    (tw, th), _ = cv2.getTextSize(
                        label, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1
                    )
                    cv2.rectangle(
                        annotated, (x1, y1 - th - 4), (x1 + tw, y1),
                        color, -1
                    )
                    cv2.putText(
                        annotated, label, (x1, y1 - 2),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1
                    )

            # 加上角标 "PC 5-CLASS"
            cv2.rectangle(annotated, (0, 0), (130, 28), (0, 100, 200), -1)
            cv2.putText(
                annotated, "PC 5-CLASS", (8, 20),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2
            )

            self.latest_frame = annotated
            self.latest_detections = detections

            # FPS 计算
            fps_frame_count += 1
            now = time.time()
            if now - last_fps_time >= 1.0:
                self.latest_fps = fps_frame_count / (now - last_fps_time)
                fps_frame_count = 0
                last_fps_time = now

    def run(self):
        """启动所有线程 + Flask"""
        # 拉流线程
        t_stream = threading.Thread(target=self._stream_reader_thread, daemon=True)
        t_stream.start()

        # 推理线程
        t_infer = threading.Thread(target=self._inference_thread, daemon=True)
        t_infer.start()

        print(f"\n{'='*50}")
        print(f"PC 推理服务启动: http://localhost:{self.port}")
        print(f"  标注流: http://localhost:{self.port}/annotated-stream")
        print(f"  状态API: http://localhost:{self.port}/api/inference")
        print(f"  ESP32:  http://{self.esp32_host}:81/stream")
        print(f"{'='*50}\n")

        self.app.run(host='0.0.0.0', port=self.port, debug=False, threaded=True)


def main():
    parser = argparse.ArgumentParser(description="PC 端 YOLO11s 推理服务")
    parser.add_argument(
        "--esp32-host",
        default="192.168.1.100",
        help="ESP32 的 IP 地址 (默认: 192.168.1.100)",
    )
    parser.add_argument(
        "--model",
        default=str(SCRIPT_DIR / "pc_5class_model.onnx"),
        help="模型文件路径 (默认: pc_5class_model.onnx)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=5000,
        help="服务端口 (默认: 5000)",
    )
    parser.add_argument(
        "--conf",
        type=float,
        default=0.25,
        help="置信度阈值 (默认: 0.25)",
    )
    parser.add_argument(
        "--iou",
        type=float,
        default=0.45,
        help="NMS IOU 阈值 (默认: 0.45)",
    )
    args = parser.parse_args()

    server = PCInferenceServer(
        esp32_host=args.esp32_host,
        model_path=args.model,
        port=args.port,
        conf_threshold=args.conf,
        iou_threshold=args.iou,
    )
    server.run()


if __name__ == "__main__":
    main()
