from flask import Flask, Response, jsonify, render_template
import cv2
import numpy as np
import requests
import threading
import time
import json
from datetime import datetime

app = Flask(__name__)
#http://10.220.120.124
#http://10.39.93.124
CAMERA_INDEX  = 0  # Use the PC's default webcam
MOCK_MODE     = False  # Set to True to enable mock mode (static image if ESP32 is unreachable)
ESP32_BOT_URL = "http://10.58.244.208"   # ESP32 bot API (same device or different — update IP)
##//http://10.39.93.124
# Shared state
##10.58.244.208
##10.58.244.208
state = {
    "latest_qr": None,
    "qr_history": [],
    "frame": None,
    "connected": False,
    "total_scans": 0,
    "last_detection_time": None,
    # Auth state — updated after operator grants access
    "auth_pending": False,
    "auth_verified_qr": None,
    "auth_notify_time": None,
    # Manual approval gate — dashboard operator must GRANT / DENY
    "qr_pending_approval": False,
    "pending_qr_data": None,
    "pending_qr_time": None,
}

state_lock = threading.Lock()

# Cabin assignments
CABIN_NAMES = {
    1: "ALLEN",
    2: "SAM",
    3: "JOSH",
    4: "NILL"
}


def fetch_frames():
    """Background thread: fetch frames from the local PC camera, detect QR, and update state."""
    detector = cv2.QRCodeDetector()
    static_img = np.zeros((240, 320, 3), dtype=np.uint8)
    cv2.putText(static_img, "MOCK MODE", (50, 120), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)

    cap = None

    while True:
        try:
            with state_lock:
                state["connected"] = False

            if MOCK_MODE:
                # Use static image for mock mode
                frame = static_img.copy()
                cv2.putText(frame, datetime.now().strftime("%H:%M:%S"), (70, 200), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255,255,255), 2)
                _, buf = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
                with state_lock:
                    state["frame"] = buf.tobytes()
                    state["connected"] = True
                time.sleep(0.2)
                continue

            if cap is None or not cap.isOpened():
                if cap is not None:
                    cap.release()
                cap = cv2.VideoCapture(CAMERA_INDEX, cv2.CAP_DSHOW)
                if not cap.isOpened():
                    print(f"[Camera] Failed to open camera index {CAMERA_INDEX}. Retrying...")
                    time.sleep(1)
                    continue

            ok, frame = cap.read()
            if not ok or frame is None:
                print("[Camera] Frame lost. Reopening camera...")
                with state_lock:
                    state["connected"] = False
                    state["frame"] = None
                cap.release()
                cap = None
                time.sleep(0.5)
                continue

            with state_lock:
                state["connected"] = True

            # QR detection
            data, bbox, _ = detector.detectAndDecode(frame)

            if bbox is not None:
                pts = np.int32(bbox).reshape(-1, 2)
                for i in range(len(pts)):
                    cv2.line(frame, tuple(pts[i]), tuple(pts[(i+1) % len(pts)]), (0, 255, 80), 2)
                if data:
                    now = datetime.now()
                    timestamp = now.strftime("%H:%M:%S")
                    date_str = now.strftime("%d %b %Y")
                    cv2.putText(frame, data[:40], (pts[0][0], max(pts[0][1]-10, 20)),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 255, 80), 2)
                    with state_lock:
                        if state["latest_qr"] != data:
                            state["total_scans"] += 1
                            entry = {
                                "id": state["total_scans"],
                                "data": data,
                                "time": timestamp,
                                "date": date_str,
                                "status": "Delivered" if state["total_scans"] % 3 != 0 else "Pending",
                            }
                            state["qr_history"].insert(0, entry)
                            if len(state["qr_history"]) > 20:
                                state["qr_history"].pop()
                            # ── Request operator approval from dashboard ──
                            state["qr_pending_approval"] = True
                            state["pending_qr_data"]     = data
                            state["pending_qr_time"]     = timestamp
                            print(f"[AUTH] New QR awaiting dashboard approval: {data}")
                        state["latest_qr"] = data
                        state["last_detection_time"] = timestamp

            # Encode to JPEG for streaming
            _, buf = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            with state_lock:
                state["frame"] = buf.tobytes()

        except Exception as e:
            print(f"[Stream Error] {e}")
            if MOCK_MODE:
                # Always show static image if ESP32 is unreachable
                frame = static_img.copy()
                cv2.putText(frame, "NO CAMERA", (70, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0,0,255), 2)
                _, buf = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
                with state_lock:
                    state["frame"] = buf.tobytes()
                    state["connected"] = False
                time.sleep(1)
            else:
                with state_lock:
                    state["connected"] = False
                    state["frame"] = None
                time.sleep(3)


def gen_frames():
    """MJPEG frame generator."""
    while True:
        with state_lock:
            frame = state["frame"]
        if frame:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        time.sleep(0.04)


@app.route('/video_feed')
def video_feed():
    return Response(gen_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')


@app.route('/api/status')
def api_status():
    with state_lock:
        return jsonify({
            "connected":            state["connected"],
            "latest_qr":            state["latest_qr"],
            "total_scans":          state["total_scans"],
            "last_detection_time":  state["last_detection_time"],
            "qr_history":           state["qr_history"][:10],
            "auth_pending":         state["auth_pending"],
            "auth_verified_qr":     state["auth_verified_qr"],
            "auth_notify_time":     state["auth_notify_time"],
            "qr_pending_approval":  state["qr_pending_approval"],
            "pending_qr_data":      state["pending_qr_data"],
            "pending_qr_time":      state["pending_qr_time"],
        })


@app.route('/')
def index():
    return render_template('index.html')


@app.route('/api/bot')
def api_bot():
    """Proxy the ESP32 hardware bot's JSON status to the dashboard."""
    try:
        r = requests.get(f"{ESP32_BOT_URL}/api/status", timeout=2)
        return r.text, r.status_code, {"Content-Type": "application/json", "Access-Control-Allow-Origin": "*"}
    except Exception as e:
        return jsonify({
            "screen": "OFFLINE",
            "line1": "Bot offline",
            "line2": str(e)[:30],
            "line3": "",
            "currentCabin": 0, "targetCabin": 0,
            "selectedCabin": 1, "selectedMode": 0,
            "deliveryActive": False, "delivered": False,
            "menuActive": False, "selectCabinMode": False
        }), 200


@app.route('/api/grant_access', methods=['POST'])
def grant_access():
    """Operator grants access — Flask notifies ESP32 bot to open cabin based on assignment."""
    with state_lock:
        qr_data = state.get("pending_qr_data")
    if not qr_data:
        return jsonify({"status": "no_pending"}), 400
    try:
        import urllib.parse
        # Try to extract cabin number or name from QR data
        cabin_number = None
        cabin_name = None
        # Example: QR data contains cabin number or name
        for num, name in CABIN_NAMES.items():
            if str(num) in qr_data or name.upper() in qr_data.upper():
                cabin_number = num
                cabin_name = name
                break
        if not cabin_number:
            # Default to 1 if not found
            cabin_number = 1
            cabin_name = CABIN_NAMES[1]
        qr_encoded = urllib.parse.quote(qr_data)
        # Pass cabin_number as a parameter if needed by ESP32_BOT_URL
        auth_url = f"{ESP32_BOT_URL}/api/auth_verify?qr={qr_encoded}&cabin={cabin_number}"
        auth_resp = requests.get(auth_url, timeout=3)
        resp_json = auth_resp.json()
        timestamp = datetime.now().strftime("%H:%M:%S")
        with state_lock:
            state["qr_pending_approval"] = False
            state["pending_qr_data"]     = None
            state["pending_qr_time"]     = None
            state["auth_verified_qr"]    = qr_data
            state["auth_notify_time"]    = timestamp
        print(f"[AUTH] Operator GRANTED access for: {qr_data} (Cabin {cabin_number}: {cabin_name})")
        return jsonify({"status": "granted", "cabin": cabin_number, "cabin_name": cabin_name, "bot_response": resp_json})
    except Exception as e:
        with state_lock:
            state["qr_pending_approval"] = False
            state["pending_qr_data"]     = None
            state["pending_qr_time"]     = None
        print(f"[AUTH] Grant failed — bot unreachable: {e}")
        return jsonify({"status": "bot_offline", "message": str(e)}), 200


@app.route('/api/deny_access', methods=['POST'])
def deny_access():
    """Operator denies access — clears pending state."""
    with state_lock:
        qr_data = state.get("pending_qr_data")
        state["qr_pending_approval"] = False
        state["pending_qr_data"]     = None
        state["pending_qr_time"]     = None
    try:
        requests.get(f"{ESP32_BOT_URL}/api/discard", timeout=2)
        print(f"[AUTH] Operator DENIED access for: {qr_data}")
        return jsonify({"status": "denied", "bot_response": "discarded"})
    except Exception as e:
        print(f"[AUTH] Deny failed — bot unreachable: {e}")
        return jsonify({"status": "bot_offline", "message": str(e)}), 200


if __name__ == '__main__':
    t = threading.Thread(target=fetch_frames, daemon=True)
    t.start()
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)
