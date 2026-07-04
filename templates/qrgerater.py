import cv2

name = input("sam")

encoder = cv2.QRCodeEncoder_create()

qr = encoder.encode(name, None)[1]

cv2.imshow("QR Code", qr)
cv2.imwrite("qr_" + name + ".png", qr)

cv2.waitKey(0)
cv2.destroyAllWindows()