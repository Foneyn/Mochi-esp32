\
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// Mochi 1.0 — ESP32-WROOM-32
// Inspiration: Plexynex Robot Face Emotion Expressions
// Visual ideas are adapted for a small monochrome OLED.

constexpr int PIN_SDA = 21;
constexpr int PIN_SCL = 18;
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t MPU_ADDR = 0x68;
constexpr int WIDTH = 128;
constexpr int HEIGHT = 64;

Adafruit_SSD1306 display(WIDTH, HEIGHT, &Wire, -1);

enum Emotion : uint8_t {
  NEUTRAL, HAPPY, DELIGHTED, SURPRISED, ANGRY,
  SAD, SLEEPY, CURIOUS, SCARED
};

Emotion emotion = NEUTRAL;
bool mpuPresent = false;
bool waking = true;
bool shakeRecovery = false;
int mood = 60;
int gazeX = 0;
int gazeY = 0;

uint32_t bootAt = 0;
uint32_t nextBlinkAt = 0;
uint32_t blinkUntil = 0;
uint32_t emotionUntil = 0;
uint32_t nextIdleAt = 0;
uint32_t recoveryUntil = 0;
uint32_t lastMpuRead = 0;
float prevAx = 0, prevAy = 0, prevAz = 0;

bool due(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

void setEmotion(Emotion e, uint32_t duration = 0) {
  emotion = e;
  emotionUntil = duration ? millis() + duration : 0;
}

int16_t readMpuWord(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)2) != 2) return 0;
  return (int16_t)((Wire.read() << 8) | Wire.read());
}

void initMpu() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  mpuPresent = (Wire.endTransmission() == 0);
  if (mpuPresent) {
    delay(30);
    prevAx = readMpuWord(0x3B) / 16384.0f;
    prevAy = readMpuWord(0x3D) / 16384.0f;
    prevAz = readMpuWord(0x3F) / 16384.0f;
  }
  Serial.println(mpuPresent ? "MPU6050 found" : "MPU6050 not found; OLED-only mode");
}

void drawEye(int x, int y, int w, int h, bool closed, bool angry) {
  if (closed || h < 4) {
    display.drawLine(x, y + h / 2, x + w, y + h / 2, SSD1306_WHITE);
    return;
  }

  display.fillRoundRect(x, y, w, h, min(9, h / 2), SSD1306_WHITE);

  int px = x + w / 2 + gazeX;
  int py = y + h / 2 + gazeY;
  display.fillCircle(px, py, min(5, h / 4), SSD1306_BLACK);
  display.drawPixel(px - 2, py - 2, SSD1306_WHITE);

  if (angry) {
    // Eyebrow strokes are drawn separately by drawFace().
  }
}

void drawStars() {
  display.drawLine(39, 8, 39, 17, SSD1306_WHITE);
  display.drawLine(35, 12, 43, 12, SSD1306_WHITE);
  display.drawLine(91, 7, 91, 16, SSD1306_WHITE);
  display.drawLine(87, 11, 95, 11, SSD1306_WHITE);
}

void drawSmile() {
  display.drawLine(54, 51, 60, 55, SSD1306_WHITE);
  display.drawLine(60, 55, 68, 55, SSD1306_WHITE);
  display.drawLine(68, 55, 74, 51, SSD1306_WHITE);
}

void renderFace() {
  display.clearDisplay();

  const uint32_t now = millis();
  const bool blinking = !due(now, blinkUntil);
  int eyeH = 27;
  if (emotion == SLEEPY) eyeH = 13;
  if (emotion == SAD || emotion == ANGRY) eyeH = 21;
  if (emotion == SURPRISED || emotion == SCARED) eyeH = 32;

  if (emotion == DELIGHTED) drawStars();

  bool closed = blinking;
  drawEye(22, 18, 36, eyeH, closed, emotion == ANGRY);
  drawEye(70, 18, 36, eyeH, closed, emotion == ANGRY);

  if (emotion == ANGRY) {
    display.drawLine(22, 14, 49, 20, SSD1306_WHITE);
    display.drawLine(79, 20, 106, 14, SSD1306_WHITE);
  } else if (emotion == SAD) {
    display.drawLine(22, 15, 48, 20, SSD1306_WHITE);
    display.drawLine(80, 20, 106, 15, SSD1306_WHITE);
  }

  if (emotion == HAPPY || emotion == DELIGHTED) drawSmile();

  if (emotion == SCARED) {
    display.drawFastHLine(5, 4, 10, SSD1306_WHITE);
    display.drawFastHLine(113, 4, 10, SSD1306_WHITE);
  }

  display.display();
}

void glitchFrame() {
  display.clearDisplay();
  for (int i = 0; i < 15; ++i) {
    int y = random(0, HEIGHT);
    int x = random(0, WIDTH);
    int w = random(5, 55);
    display.drawFastHLine(x, y, min(w, WIDTH - x), SSD1306_WHITE);
  }
  if (random(0, 3) == 0) {
    display.fillRect(random(0, 105), random(0, 57),
                     random(4, 22), random(2, 7), SSD1306_WHITE);
  }
  display.display();
}

void startupAnimation() {
  uint32_t t = millis() - bootAt;

  if (t < 1000) {
    glitchFrame();
  } else if (t < 1450) {
    display.clearDisplay();
    display.display();
  } else if (t < 2300) {
    emotion = SLEEPY;
    blinkUntil = 0;
    renderFace();
  } else if (t < 3300) {
    emotion = NEUTRAL;
    renderFace();
  } else {
    waking = false;
    emotion = NEUTRAL;
    nextBlinkAt = millis() + random(2200, 5000);
    nextIdleAt = millis() + 4500;
  }
}

void updateMotion() {
  uint32_t now = millis();
  if (!mpuPresent || now - lastMpuRead < 40) return;
  lastMpuRead = now;

  float ax = readMpuWord(0x3B) / 16384.0f;
  float ay = readMpuWord(0x3D) / 16384.0f;
  float az = readMpuWord(0x3F) / 16384.0f;
  float delta = fabsf(ax - prevAx) + fabsf(ay - prevAy) + fabsf(az - prevAz);
  prevAx = ax; prevAy = ay; prevAz = az;

  if (delta > 0.85f && !shakeRecovery) {
    setEmotion(SCARED, 1500);
    mood = max(0, mood - 8);
    shakeRecovery = true;
    recoveryUntil = now + 1500;
    return;
  }

  // Gentle tilt: glance toward the tilt direction.
  if (!shakeRecovery) {
    if (ax > 0.55f) { gazeX = 5; setEmotion(CURIOUS, 650); }
    else if (ax < -0.55f) { gazeX = -5; setEmotion(CURIOUS, 650); }
    else if (ay > 0.60f) { gazeY = 3; }
    else { gazeX = 0; gazeY = 0; }
  }
}

void updateCharacter() {
  uint32_t now = millis();

  if (shakeRecovery && due(now, recoveryUntil)) {
    shakeRecovery = false;
    setEmotion(SAD, 3500);
  }

  if (emotionUntil && due(now, emotionUntil)) {
    emotionUntil = 0;
    emotion = NEUTRAL;
  }

  if (due(now, nextBlinkAt)) {
    blinkUntil = now + 130;
    nextBlinkAt = now + random(2500, 6500);
  }

  if (!shakeRecovery && !emotionUntil && due(now, nextIdleAt)) {
    switch (random(0, 5)) {
      case 0: setEmotion(CURIOUS, 1400); gazeX = random(-5, 6); break;
      case 1: setEmotion(HAPPY, 1600); mood = min(100, mood + 2); break;
      case 2: setEmotion(SLEEPY, 1800); break;
      case 3: setEmotion(DELIGHTED, 1500); mood = min(100, mood + 3); break;
      default: setEmotion(NEUTRAL, 1200); gazeX = 0; gazeY = 0; break;
    }
    nextIdleAt = now + random(7000, 14000);
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case '0': setEmotion(NEUTRAL); break;
    case '1': setEmotion(HAPPY); break;
    case '2': setEmotion(ANGRY); break;
    case '3': setEmotion(SURPRISED); break;
    case '4': setEmotion(SLEEPY); break;
    case '5': setEmotion(SAD); break;
    case '6': setEmotion(DELIGHTED); break;
    case '7': setEmotion(CURIOUS); break;
    case 'b': blinkUntil = millis() + 180; break;
    case 'l': gazeX = -5; break;
    case 'r': gazeX = 5; break;
    case 'c': gazeX = 0; gazeY = 0; break;
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed. Check display driver/address.");
    while (true) delay(100);
  }

  display.clearDisplay();
  display.display();
  initMpu();

  bootAt = millis();
  Serial.println("Mochi 1.0 starting");
  Serial.println("Serial: 0 neutral, 1 happy, 2 angry, 3 surprised,");
  Serial.println("4 sleepy, 5 sad, 6 delighted, 7 curious, b blink, l/r/c gaze");
}

void loop() {
  if (waking) {
    startupAnimation();
  } else {
    updateMotion();
    updateCharacter();
    renderFace();
  }

  handleSerial();
  delay(35);
}
