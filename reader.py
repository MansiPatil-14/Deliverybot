import cv2
import numpy as np
import requests
#http://10.220.120.124
url = "http://10.220.120.124/stream"   # ESP32 stream URL

detector = cv2.QRCodeDetector()

stream = requests.get(url, stream=True)
bytes_data = bytes()

while True:
    for chunk in stream.iter_content(chunk_size=1024):
        bytes_data += chunk
        a = bytes_data.find(b'\xff\xd8')
        b = bytes_data.find(b'\xff\xd9')

        if a != -1 and b != -1:
            jpg = bytes_data[a:b+2]
            bytes_data = bytes_data[b+2:]

            frame = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)

            # 🔍 Detect QR
            data, bbox, _ = detector.detectAndDecode(frame)

            if bbox is not None:
                pts = np.int32(bbox).reshape(-1, 2)

                for i in range(len(pts)):
                    cv2.line(frame, tuple(pts[i]), tuple(pts[(i+1) % len(pts)]), (0,255,0), 2)

                if data:
                    print("QR Code:", data)
                    cv2.putText(frame, data, (pts[0][0], pts[0][1]-10),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0,255,0), 2)

            cv2.imshow("ESP32 QR Scanner (OpenCV)", frame)

            if cv2.waitKey(1) == 27:
                exit()