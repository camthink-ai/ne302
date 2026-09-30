#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NE302 face-detection demo - mock recognition backend.

Simulates the cloud face-recognition application described in
Docs/Face_Detection_Demo.md section 5, so the device demo can be
end-to-end tested without a real backend.

Modes (--mode):
  webhook  HTTP server, receives the device webhook multipart POST
           (parts: "metadata" JSON + "image" JPEG) and answers with the
           recognition result JSON synchronously (HTTP 200).
  mqtt     Subscribes to the face report topic, decodes the embedded
           base64 JPEG, publishes the result JSON to the result topic.
  both     Run the two servers above simultaneously (for bench tests;
           the device itself only uses one channel at a time).

Recognition strategy (--recognizer auto|face|ahash|md5):
  face   Real face recognition via insightface (ArcFace embeddings):
         detects the face in the crop, extracts a 512-D embedding and
         matches it against the gallery by cosine similarity. Needs
         `pip install insightface onnxruntime` (first run downloads the
         model pack to ~/.insightface). Crops with no detectable face
         return status "no_face".
  ahash  8x8 average hash matching with Pillow (structural, coarse).
  md5    Deterministic pseudo identity from image bytes (chain demo).
  auto   picks the best available: face > ahash > md5.
  Enrolled images live in --gallery (filename = person name, e.g.
  "zhangsan.jpg"); embeddings are cached in <gallery>/.fd_cache.pkl and
  recomputed only when files change.

Requires: Python 3.8+; paho-mqtt only for --mode mqtt/both; Pillow optional.
"""

import argparse
import base64
import hashlib
import io
import json
import os
import random
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:  # stdlib-only fallback
    HAS_PIL = False

try:
    import paho.mqtt.client as mqtt
    HAS_PAHO = True
except ImportError:
    HAS_PAHO = False

try:
    import numpy as _np  # insightface dependency
    from insightface.app import FaceAnalysis
    HAS_INSIGHTFACE = True
except Exception:  # noqa: BLE001 - heavy optional dep (onnxruntime etc.)
    HAS_INSIGHTFACE = False

FALLBACK_NAMES = ["Alice", "Bob", "Carol", "Dave", "Eve",
                  "Frank", "Grace", "Heidi", "Ivan", "Judy"]

RESULT_TYPE = "face_recognition_result"
REPORT_TYPE = "face_report"


def now_ms():
    return int(time.time() * 1000)


def log(tag, msg):
    print("[%s] [%s] %s" % (time.strftime("%H:%M:%S"), tag, msg), flush=True)


# --------------------------------------------------------------------------
# Recognition
# --------------------------------------------------------------------------

def ahash_pil(img_bytes):
    """8x8 grayscale average hash -> 64-bit int."""
    img = Image.open(io.BytesIO(img_bytes)).convert("L").resize((8, 8), Image.LANCZOS)
    pixels = list(img.getdata())
    avg = sum(pixels) / len(pixels)
    bits = 0
    for p in pixels:
        bits = (bits << 1) | (1 if p >= avg else 0)
    return bits


def hamming(a, b):
    return bin(a ^ b).count("1")


def _cosine(a, b):
    import math
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    if na == 0.0 or nb == 0.0:
        return 0.0
    return dot / (na * nb)


class Recognizer(object):
    """Tiered recognizer: face (ArcFace) > ahash > md5 pseudo identity."""

    def __init__(self, args):
        self.args = args
        self.gallery = []      # (name, hash) for ahash mode
        self.embeddings = []   # (name, vector) for face mode
        self.face_app = None
        self.no_face = False

        mode = args.recognizer
        if mode == "auto":
            mode = "face" if HAS_INSIGHTFACE else ("ahash" if HAS_PIL else "md5")
        self.mode = mode

        if self.mode == "face" and not HAS_INSIGHTFACE:
            log("recognizer", "insightface unavailable, falling back")
            self.mode = "ahash" if HAS_PIL else "md5"
        if self.mode == "ahash" and not HAS_PIL:
            self.mode = "md5"

        if self.mode == "face":
            log("recognizer", "loading ArcFace model pack "
                "(first run downloads ~330MB to ~/.insightface)...")
            self.face_app = FaceAnalysis(name="buffalo_l",
                                         allowed_modules=["detection", "recognition"])
            self.face_app.prepare(ctx_id=0, det_size=(320, 320))
            self._load_gallery_faces(args.gallery)
        elif self.mode == "ahash":
            self._load_gallery_hashes(args.gallery)

        log("recognizer", "mode=%s gallery=%d entries"
            % (self.mode, len(self.gallery) + len(self.embeddings)))

    # ---- gallery loading -------------------------------------------------

    def _gallery_files(self, gallery):
        """Yield (person_name, image_path). Two layouts are supported:
        <gallery>/<name>.jpg              (single shot)
        <gallery>/<person_dir>/*.jpg      (multi shot, name = dir name)"""
        files = []
        if gallery and os.path.isdir(gallery):
            for fn in sorted(os.listdir(gallery)):
                path = os.path.join(gallery, fn)
                if fn.startswith("."):
                    continue
                if os.path.isfile(path):
                    files.append((os.path.splitext(fn)[0], path))
                elif os.path.isdir(path):
                    for sub in sorted(os.listdir(path)):
                        sub_path = os.path.join(path, sub)
                        if os.path.isfile(sub_path) and not sub.startswith("."):
                            files.append((fn, sub_path))
        return files

    def _load_gallery_hashes(self, gallery):
        for name, path in self._gallery_files(gallery):
            try:
                with open(path, "rb") as f:
                    self.gallery.append((name, ahash_pil(f.read())))
            except Exception as e:  # noqa: BLE001
                log("gallery", "skip %s: %s" % (name, e))

    def _load_gallery_faces(self, gallery):
        """Extract embeddings once, cache in <gallery>/.fd_cache.pkl keyed
        by file mtime so re-runs are instant unless files changed."""
        import pickle
        cache_path = None
        cache = {}
        if gallery and os.path.isdir(gallery):
            cache_path = os.path.join(gallery, ".fd_cache.pkl")
            try:
                with open(cache_path, "rb") as f:
                    cache = pickle.load(f)
            except Exception:  # noqa: BLE001
                cache = {}
        for name, path in self._gallery_files(gallery):
            try:
                mtime = os.path.getmtime(path)
                cached = cache.get(path)
                if cached and cached[0] == mtime:
                    emb = cached[1]
                else:
                    with open(path, "rb") as f:
                        emb = self._embed_bytes(f.read())
                    if emb is None:
                        log("gallery", "no face found in %s - skipped" % name)
                        continue
                    cache[path] = (mtime, emb)
                self.embeddings.append((name, emb))
            except Exception as e:  # noqa: BLE001
                log("gallery", "skip %s: %s" % (name, e))
        if cache_path:
            try:
                with open(cache_path, "wb") as f:
                    pickle.dump(cache, f)
            except OSError as e:
                log("gallery", "cache write failed: %s" % e)

    # ---- inference -------------------------------------------------------

    def _embed_bytes(self, jpeg_bytes):
        """512-D embedding of the first face in the crop, or None."""
        import numpy as np
        import cv2
        arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if img is None:
            return None
        faces = self.face_app.get(img)
        if not faces:
            return None
        return faces[0].normed_embedding.tolist()

    def recognize(self, jpeg_bytes):
        """Return (name, confidence) or (None, 0.0) for unknown."""
        if random.random() < self.args.unknown_rate:
            return None, 0.0

        if self.mode == "face":
            try:
                emb = self._embed_bytes(jpeg_bytes)
            except Exception as e:  # noqa: BLE001
                log("recognize", "embedding failed: %s" % e)
                emb = None
            if emb is None:
                self.no_face = True
                return None, 0.0
            best_name, best_sim = None, -1.0
            for name, ref in self.embeddings:
                sim = _cosine(emb, ref)
                if sim > best_sim:
                    best_name, best_sim = name, sim
            if best_name is not None and best_sim >= self.args.match_threshold:
                conf = max(0.60, min(0.99, 0.5 + best_sim / 2.0))
                return best_name, round(conf, 2)
            return None, 0.0

        if self.mode == "ahash":
            try:
                crop_hash = ahash_pil(jpeg_bytes)
            except Exception:  # noqa: BLE001
                return None, 0.0
            best_name, best_dist = None, 64
            for name, gh in self.gallery:
                d = hamming(crop_hash, gh)
                if d < best_dist:
                    best_name, best_dist = name, d
            if best_name is not None and best_dist <= self.args.hash_threshold:
                conf = max(0.55, min(0.99, 1.0 - best_dist / 64.0))
                return best_name, round(conf, 2)
            return None, 0.0

        # md5 pseudo identity (chain demo only)
        digest = hashlib.md5(jpeg_bytes).digest()
        name = FALLBACK_NAMES[digest[2] % len(FALLBACK_NAMES)]
        conf = 0.70 + (digest[3] % 30) / 100.0
        return name, round(conf, 2)


# --------------------------------------------------------------------------
# Shared report processing
# --------------------------------------------------------------------------

class ReportProcessor(object):
    def __init__(self, args):
        self.args = args
        self.recognizer = Recognizer(args)
        os.makedirs(args.out, exist_ok=True)
        self.lock = threading.Lock()
        self.stats = {"received": 0, "ok": 0, "unknown": 0, "error": 0, "saved": 0}

    def handle(self, metadata, jpeg_bytes):
        """Process one face report, return the result dict (schema §5.3)."""
        with self.lock:
            self.stats["received"] += 1

        request_id = metadata.get("request_id")
        face = metadata.get("face") or {}
        track_id = face.get("track_id", -1)
        ts = metadata.get("timestamp_ms", now_ms())
        reason = face.get("reason", "")

        log("report", "request_id=%s track_id=%s reason=%s bytes=%s"
            % (request_id, track_id, reason, len(jpeg_bytes) if jpeg_bytes else 0))

        if self.args.latency_ms:
            time.sleep(self.args.latency_ms / 1000.0)

        if random.random() < self.args.fail_rate:
            with self.lock:
                self.stats["error"] += 1
            return build_result(request_id, track_id, ts, "error", "", 0.0,
                                "simulated backend failure")

        # save crop for debugging / enrollment
        saved_as = None
        if jpeg_bytes:
            try:
                fn = "%d_t%s_%s.jpg" % (now_ms(), track_id,
                                        re.sub(r"[^A-Za-z0-9_.-]", "", str(request_id))[-24:] or "req")
                with open(os.path.join(self.args.out, fn), "wb") as f:
                    f.write(jpeg_bytes)
                saved_as = fn
                with self.lock:
                    self.stats["saved"] += 1
            except OSError as e:
                log("save", "failed to save crop: %s" % e)

        if not jpeg_bytes:
            with self.lock:
                self.stats["error"] += 1
            return build_result(request_id, track_id, ts, "error", "", 0.0,
                                "report contains no image data")

        name, conf = self.recognizer.recognize(jpeg_bytes)
        # recognize() sets no_face for THIS call (face mode only)
        no_face = getattr(self.recognizer, "no_face", False)
        self.recognizer.no_face = False
        if name is None:
            status = "no_face" if no_face else "unknown"
            with self.lock:
                self.stats["unknown"] += 1
            log("result", "request_id=%s -> %s%s"
                % (request_id, status.upper(),
                   " (%s)" % saved_as if saved_as else ""))
            return build_result(request_id, track_id, ts, status, "unknown", 0.0)

        with self.lock:
            self.stats["ok"] += 1
        log("result", "request_id=%s -> %s (%.2f)%s"
            % (request_id, name, conf, " (%s)" % saved_as if saved_as else ""))
        return build_result(request_id, track_id, ts, "ok", name, conf)


def build_result(request_id, track_id, timestamp_ms, status, name, confidence,
                 message=""):
    return {
        "type": RESULT_TYPE,
        "request_id": request_id,
        "track_id": track_id,
        "timestamp_ms": timestamp_ms,
        "status": status,
        "name": name,
        "confidence": confidence,
        "message": message,
    }


# --------------------------------------------------------------------------
# Payload parsing (webhook multipart + mqtt json envelope)
# --------------------------------------------------------------------------

def parse_multipart(body, boundary):
    """Return dict {name: bytes} of form parts."""
    parts = {}
    delim = b"--" + boundary
    for chunk in body.split(delim):
        chunk = chunk.strip(b"\r\n")
        if not chunk or chunk == b"--":
            continue
        if b"\r\n\r\n" not in chunk:
            continue
        raw_headers, content = chunk.split(b"\r\n\r\n", 1)
        headers = raw_headers.decode("utf-8", "replace")
        m = re.search(r'name="([^"]+)"', headers)
        if m:
            parts[m.group(1)] = content
    return parts


def extract_data_uri_b64(value):
    """'data:image/jpeg;base64,xxxx' -> bytes, or None."""
    if not isinstance(value, str):
        return None
    if "," in value:
        value = value.split(",", 1)[1]
    try:
        return base64.b64decode(value, validate=False)
    except Exception:  # noqa: BLE001
        return None


# --------------------------------------------------------------------------
# Webhook mode
# --------------------------------------------------------------------------

class WebhookHandler(BaseHTTPRequestHandler):
    server_version = "NE302FaceMock/1.0"
    processor = None  # injected
    args = None

    def log_message(self, fmt, *a):  # quiet default access log
        pass

    def _send_json(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in ("/", "/health"):
            self._send_json(200, {
                "service": "ne302-face-mock-backend",
                "mode": "webhook",
                "endpoints": {"POST /face": "device webhook (multipart or json)"},
                "stats": self.processor.stats,
            })
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self):
        if self.path.rstrip("/") not in ("/face", "/webhook", "/api/face"):
            self._send_json(404, {"error": "not found, POST /face"})
            return

        try:
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""
        except (ValueError, OSError):
            self._send_json(400, {"error": "bad body"})
            return

        metadata, jpeg = self._parse_body(body)
        if metadata is None:
            self._send_json(400, {"error": "unrecognized payload"})
            return

        result = self.processor.handle(metadata, jpeg)
        # error simulation also surfaces as HTTP 500 so devices can exercise
        # their non-200 handling path
        code = 500 if result.get("status") == "error" and self.args.fail_as_500 else 200
        self._send_json(code, result)

    def _parse_body(self, body):
        ctype = self.headers.get("Content-Type", "")
        m = re.search(r'boundary="?([^";]+)"?', ctype)
        if "multipart/form-data" in ctype and m:
            parts = parse_multipart(body, m.group(1).encode())
            meta_raw = parts.get("metadata")
            jpeg = parts.get("image")
            if meta_raw:
                try:
                    return json.loads(meta_raw.decode("utf-8")), jpeg
                except (UnicodeDecodeError, json.JSONDecodeError):
                    return None, None
            return None, None

        # plain JSON envelope (same as MQTT payload, with image_data inside)
        try:
            envelope = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None, None
        jpeg = extract_data_uri_b64(envelope.get("image_data"))
        envelope.pop("image_data", None)
        envelope.pop("encoding", None)
        return envelope, jpeg


def run_webhook(args, processor):
    handler = type("BoundHandler", (WebhookHandler,),
                   {"processor": processor, "args": args})
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    log("webhook", "listening on http://%s:%d/face (POST)" %
        (args.host if args.host != "0.0.0.0" else "0.0.0.0", args.port))
    httpd.serve_forever()


# --------------------------------------------------------------------------
# MQTT mode
# --------------------------------------------------------------------------

def run_mqtt(args, processor):
    if not HAS_PAHO:
        log("mqtt", "paho-mqtt not installed: pip install paho-mqtt")
        sys.exit(2)

    def derive_result_topic(report_topic):
        if args.topic_result:
            return args.topic_result
        if report_topic.endswith("/report"):
            return report_topic[:-len("/report")] + "/result"
        return report_topic + "/result"

    def on_connect(client, userdata, flags, rc, *extra):
        client.subscribe(args.topic_report)
        log("mqtt", "connected (rc=%s), subscribed: %s" % (rc, args.topic_report))

    def on_message(client, userdata, msg):
        try:
            envelope = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            log("mqtt", "malformed json on %s (%d bytes)" % (msg.topic, len(msg.payload)))
            return
        jpeg = extract_data_uri_b64(envelope.pop("image_data", None))
        envelope.pop("encoding", None)
        result = processor.handle(envelope, jpeg)
        topic = derive_result_topic(msg.topic)
        client.publish(topic, json.dumps(result, ensure_ascii=False), qos=1)
        log("mqtt", "result published to %s (status=%s)"
            % (topic, result.get("status")))

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    except AttributeError:  # paho-mqtt 1.x
        client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    if args.mqtt_user:
        client.username_pw_set(args.mqtt_user, args.mqtt_password)
    client.connect(args.mqtt_host, args.mqtt_port, keepalive=60)
    client.loop_forever()


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(
        description="NE302 face-detection demo mock backend (see "
                    "Docs/Face_Detection_Demo.md)")
    p.add_argument("--mode", choices=("webhook", "mqtt", "both"), default="webhook")
    p.add_argument("--host", default="0.0.0.0", help="webhook listen host")
    p.add_argument("--port", type=int, default=8080, help="webhook listen port")
    p.add_argument("--mqtt-host", default="127.0.0.1")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--mqtt-user", default=None)
    p.add_argument("--mqtt-password", default=None)
    p.add_argument("--topic-report", default="devices/+/face/report",
                   help="topic to subscribe for device reports")
    p.add_argument("--topic-result", default=None,
                   help="result topic (default: report topic with /result suffix)")
    p.add_argument("--out", default=os.path.join(".", "face_captures"),
                   help="directory to save received face crops")
    p.add_argument("--gallery", default=None,
                   help="directory of enrolled faces, filename = person name")
    p.add_argument("--hash-threshold", type=int, default=12,
                   help="aHash hamming distance threshold (of 64)")
    p.add_argument("--recognizer", choices=("auto", "face", "ahash", "md5"),
                   default="auto",
                   help="face=ArcFace via insightface; auto picks the best "
                        "available (face > ahash > md5)")
    p.add_argument("--match-threshold", type=float, default=0.40,
                   help="ArcFace cosine similarity threshold for a match")
    p.add_argument("--latency-ms", type=int, default=0,
                   help="simulated recognition latency")
    p.add_argument("--fail-rate", type=float, default=0.0,
                   help="probability of simulated backend error")
    p.add_argument("--fail-as-500", action="store_true",
                   help="report errors as HTTP 500 instead of status=error")
    p.add_argument("--unknown-rate", type=float, default=0.15,
                   help="probability of forcing an unknown result")
    return p.parse_args()


def main():
    args = parse_args()
    processor = ReportProcessor(args)

    log("main", "NE302 face mock backend starting (mode=%s)" % args.mode)
    r = processor.recognizer
    if r.mode == "face":
        how = "ArcFace embeddings (%d enrolled)" % len(r.embeddings)
    elif r.mode == "ahash":
        how = "aHash gallery (%d enrolled)" % len(r.gallery)
    else:
        how = "md5 pseudo-identity (chain demo; --gallery for real matching)"
    log("main", "recognition: %s" % how)
    log("main", "crops saved to: %s (use them as gallery entries to enroll)"
        % os.path.abspath(args.out))

    threads = []
    if args.mode in ("webhook", "both"):
        t = threading.Thread(target=run_webhook, args=(args, processor), daemon=True)
        t.start()
        threads.append(t)
    if args.mode in ("mqtt", "both"):
        t = threading.Thread(target=run_mqtt, args=(args, processor), daemon=True)
        t.start()
        threads.append(t)

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        log("main", "stats: %s" % processor.stats)


if __name__ == "__main__":
    main()
