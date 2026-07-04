#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WebServer.h>

// --- WiFi Configuration ---
const char* WIFI_SSID = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";

// --- Multi-Servo Setup ---
int servoPins[4] = {13, 15, 2, 4}; 
Servo cabinServos[4];

#define SERVO_CLOSED 50
#define SERVO_OPEN 0

WebServer server(80);
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// --- Pin Definitions ---
#define MIDDLE_IR 5
#define LEFT_IR 18
#define RIGHT_IR 19
#define IN1 27
#define IN2 26
#define IN3 25
#define IN4 33
#define BTN1 12
#define BTN2 14

// --- State Variables ---
int currentCabin = 0;
int targetCabin = 0;
bool deliveryActive = false;
bool awaitingAuth = false;
unsigned long authStart = 0;

#define AUTH_TIMEOUT 60000 

int selectedMode = 0; 
int selectedCabin = 1;
bool menuActive = false;
bool selectCabinMode = false;
bool lastMid = LOW;
unsigned long irHighStart = 0;   // tracks when IR first went HIGH
bool irCounted = false;          // prevents double-counting same white strip
String oledScreen = "BOOT";

// --- OLED Helper ---
void oled(String l1, String l2, String l3, String screen) {
  oledScreen = screen;
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(l1);
  display.setTextSize(1);
  display.println(l2);
  display.println(l3);
  display.display();
}

// --- Smooth Servo Logic ---
void moveRespectiveSmoothly(int cabinNum, int startAngle, int endAngle) {
  if (cabinNum < 1 || cabinNum > 4) return;
  int idx = cabinNum - 1; 
  
  if (startAngle < endAngle) {
    for (int pos = startAngle; pos <= endAngle; pos++) {
      cabinServos[idx].write(pos);
      delay(20); 
    }
  } else {
    for (int pos = startAngle; pos >= endAngle; pos--) {
      cabinServos[idx].write(pos);
      delay(20);
    }
  }
}

void openRespectiveCabin(int cabinNum) {
  oled("ACTION", "Opening Cabin " + String(cabinNum), "5 Seconds", "SERVO");
  moveRespectiveSmoothly(cabinNum, SERVO_CLOSED, SERVO_OPEN);
  delay(5000);
  moveRespectiveSmoothly(cabinNum, SERVO_OPEN, SERVO_CLOSED);
}

// --- Motor Controls ---
void stopMotors() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void moveForward() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void moveBackward() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
}

void turnLeft() {
  // Left motor stop, Right motor forward
  digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void turnRight() {
  // Left motor forward, Right motor stop
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);
}

// --- Navigation ---
void handleIR() {
  int mid = digitalRead(MIDDLE_IR);

  if (mid == HIGH) {
    if (lastMid == LOW) {
      // IR just went HIGH — start the 1-second timer
      irHighStart = millis();
      irCounted = false;
    }
    // Still HIGH: check if it has been sustained for 1 second
    if (!irCounted && (millis() - irHighStart >= 1000)) {
      // Confirmed white strip — count the cabin
      if (deliveryActive) {
        if (targetCabin > currentCabin) currentCabin++;
        else if (targetCabin < currentCabin) currentCabin--;
      } else {
        currentCabin++;
        if (currentCabin > 4) currentCabin = 1;
      }
      irCounted = true; // only count once per continuous HIGH segment
    }
  } else {
    // IR went LOW — reset for next strip
    irCounted = false;
  }

  lastMid = mid;
}

void moveToTarget() {
  if (currentCabin != targetCabin) {
    // If target cabin is behind current cabin, simply move backward
    if (targetCabin < currentCabin) {
      oled("GOING", "To: " + String(targetCabin), "Now: " + String(currentCabin), "MOVING");
      moveBackward();
    } else {
      // Forward movement with line‑following correction
      int leftVal  = digitalRead(LEFT_IR);   // LOW = black detected
      int rightVal = digitalRead(RIGHT_IR);  // LOW = black detected
      oled("GOING", "To: " + String(targetCabin), "Now: " + String(currentCabin), "MOVING");
      // Line‑following correction
      if (rightVal == LOW && leftVal == HIGH) {
        // Right sensor sees black edge → drift right → correct left
        turnLeft();
      } else if (leftVal == LOW && rightVal == HIGH) {
        // Left sensor sees black edge → drift left → correct right
        turnRight();
      } else {
        // Both clear or both on line → go straight forward
        moveForward();
      }
    }
    return;
  }
  stopMotors();
  oled("REACHED", "Cabin " + String(targetCabin), "Waiting Unlock", "ARRIVED");
  delay(2000);
  awaitingAuth = true;
  authStart = millis();
}


// --- Buttons ---
void handleButtons() {
  if (digitalRead(BTN1) == LOW) {
    delay(200);
    if (!menuActive) menuActive = true;
    else if (!selectCabinMode) selectedMode = (selectedMode + 1) % 2;
    else {
      selectedCabin++;
      if (selectedCabin > 4) selectedCabin = 1;
    }
  }

  if (digitalRead(BTN2) == LOW) {
    unsigned long t = millis();
    while (digitalRead(BTN2) == LOW);
    if (millis() - t > 800) { 
      if (selectedMode == 1) { 
        currentCabin = 0;
        oled("RESET", "Counters Cleared", "", "RESET");
        delay(1000);
        menuActive = false;
      } else { 
        if (!selectCabinMode) selectCabinMode = true;
        else {
          targetCabin = selectedCabin;
          openRespectiveCabin(targetCabin);
          deliveryActive = true;
          menuActive = false;
          selectCabinMode = false;
        }
      }
    }
  }

  if (menuActive) {
    if (!selectCabinMode) {
      oled("MENU", selectedMode == 0 ? ">SEND" : " SEND", selectedMode == 1 ? ">RESET" : " RESET", "MENU");
    } else {
      oled("SELECT", "Cabin " + String(selectedCabin), "Hold BTN2 to GO", "SELECT");
    }
  }
}

// --- API ---
void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += " \"currentCabin\":" + String(currentCabin) + ",";
  json += " \"targetCabin\":" + String(targetCabin) + ",";
  json += " \"deliveryActive\":" + String(deliveryActive ? "true" : "false") + ",";
  json += " \"awaitingAuth\":" + String(awaitingAuth ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleUnlock() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (awaitingAuth) {
    awaitingAuth = false;
    deliveryActive = false;
    openRespectiveCabin(targetCabin);
    targetCabin = 0;
    server.send(200, "application/json", "{\"status\":\"unlocked\"}");
  } else {
    server.send(403, "application/json", "{\"status\":\"denied\"}");
  }
}

void handleDiscard() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  awaitingAuth = false;
  deliveryActive = false;
  targetCabin = 0;
  oled("DISCARD", "Return to IDLE", "", "IDLE");
  server.send(200, "application/json", "{\"status\":\"discarded\"}");
}

void handleAuthVerify() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("cabin")) {
    server.send(400, "application/json", "{\"status\":\"missing_cabin\"}");
    return;
  }
  int cabinNum = server.arg("cabin").toInt();
  if (cabinNum < 1 || cabinNum > 4) {
    server.send(400, "application/json", "{\"status\":\"invalid_cabin\"}");
    return;
  }
  if (!awaitingAuth || targetCabin == 0) {
    server.send(409, "application/json", "{\"status\":\"not_waiting\"}");
    return;
  }
  if (cabinNum != targetCabin) {
    server.send(409, "application/json", "{\"status\":\"cabin_mismatch\"}");
    return;
  }

  awaitingAuth = false;
  deliveryActive = false;
  openRespectiveCabin(cabinNum);
  targetCabin = 0;
  server.send(200, "application/json", "{\"status\":\"unlocked\"}");
}


// --- Setup ---
void setup() {
  Serial.begin(115200);

  pinMode(MIDDLE_IR, INPUT);
  pinMode(LEFT_IR, INPUT);
  pinMode(RIGHT_IR, INPUT);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(BTN1, INPUT_PULLUP); pinMode(BTN2, INPUT_PULLUP);

  for(int i=0; i<4; i++) {
    cabinServos[i].attach(servoPins[i]);
    for(int pos=0; pos<=SERVO_CLOSED; pos++) {
        cabinServos[i].write(pos);
        delay(10);
    }
    delay(100); 
  }

  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  oled("BOOTING", "WiFi Connect", "", "BOOT");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  String ipAddr = WiFi.localIP().toString();

  // ✅ PRINT URL IN SERIAL
  Serial.println("\n------------------------------");
  Serial.print("SERVER URL: http://");
  Serial.print(ipAddr);
  Serial.println("/api/status");
  Serial.println("------------------------------\n");

  oled("ONLINE", "IP:", ipAddr, "IDLE");
  delay(2000); 

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/unlock", HTTP_ANY, handleUnlock); 
  server.on("/api/discard", HTTP_ANY, handleDiscard);
  server.on("/api/auth_verify", HTTP_ANY, handleAuthVerify);
  server.begin();
}

// --- Loop ---
void loop() {
  server.handleClient();
  handleIR();
  handleButtons();

  if (awaitingAuth) {
    if (millis() - authStart > AUTH_TIMEOUT) {
      awaitingAuth = false;
      deliveryActive = false;
      targetCabin = 0;
      oled("TIMEOUT", "Return to", "IDLE", "IDLE");
      delay(2000);
    }
    return;
  }

  if (deliveryActive) {
    moveToTarget();
  } else if (!menuActive) {
    stopMotors();
    oled("IDLE", "At Cabin: " + String(currentCabin), WiFi.localIP().toString(), "IDLE");
  }
}