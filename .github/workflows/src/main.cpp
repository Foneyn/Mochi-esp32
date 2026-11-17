#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "eyes.h"

// ESP32-WROOM-32 + OLED SSD1306 + MPU-6050 (GY-521)
static constexpr int I2C_SDA = 21;
static constexpr int I2C_SCL = 18;
static constexpr uint8_t OLED_ADDR = 0x3C;
static constexpr uint8_t MPU_ADDR = 0x68; // AD0 low; if AD0 high, address is 0x69
static constexpr int W = 128, H = 64;

Adafruit_SSD1306 display(W, H, &Wire, -1);
uint8_t frameBuffer[W * H / 8];

enum Mood : uint8_t { NEUTRAL, HAPPY, ANGRY, SURPRISED, SLEEPY };
Mood mood = NEUTRAL;
float gazeX = 0, gazeY = 0, targetX = 0, targetY = 0;
unsigned long nextBlink = 0, nextGlance = 0, lastDraw = 0;
bool mpuReady = false;
unsigned long lastMpuRead = 0, surpriseUntil = 0, lastShake = 0;
float filteredRoll = 0, filteredPitch = 0;

bool mpuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool mpuReadBytes(uint8_t reg, uint8_t *data, size_t count) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)MPU_ADDR, (int)count, (int)true);
  if (got != count) return false;
  for (size_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}

bool initMPU() {
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) return false;
  // Wake up, use the X gyro as clock, and enable a modest low-pass filter.
  if (!mpuWrite(0x6B, 0x01)) return false; // PWR_MGMT_1
  mpuWrite(0x1A, 0x03); // DLPF ~44 Hz accel / 42 Hz gyro
  mpuWrite(0x1B, 0x00); // gyro +/-250 deg/s
  mpuWrite(0x1C, 0x00); // accel +/-2 g
  delay(100);
  return true;
}

int16_t readI16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] << 8 | p[1]); }

void showBuffer() {
  display.clearDisplay();
  display.drawBitmap(0, 0, frameBuffer, W, H, SSD1306_WHITE);
  display.display();
}

void renderMood() {
  switch (mood) {
    case HAPPY:     Eyes::draw_open(gazeY, gazeX, 0.0f, 0.20f, 3.0f, frameBuffer); break;
    case ANGRY:     Eyes::draw_open(gazeY, gazeX, 0.75f, 0.55f, -9.0f, frameBuffer); break;
    case SURPRISED: Eyes::draw_open(gazeY, gazeX, 0.0f, 0.95f, 9.0f, frameBuffer); break;
    case SLEEPY:    Eyes::draw_half_open(frameBuffer); break;
    default:        Eyes::draw_open(gazeY, gazeX, 0.0f, 0.30f, 0.0f, frameBuffer); break;
  }
  showBuffer();
}

void blinkNow() {
  Eyes::draw_closed(frameBuffer); showBuffer();
  delay(110);
  if (mood != SLEEPY && millis() >= surpriseUntil) renderMood();
  nextBlink = millis() + random(2200, 5200);
}

void printHelp() {
  Serial.println(F("\nMochi + MPU6050 ready"));
  Serial.println(F("0 neutral | 1 happy | 2 angry | 3 surprised | 4 sleepy"));
  Serial.println(F("b blink | l left | r right | c center | ? help"));
  Serial.println(F("Tilt Mochi to move its gaze; shake to surprise it."));
  Serial.println(mpuReady ? F("MPU6050: detected at 0x68") : F("MPU6050: NOT detected; check wiring/address"));
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case '0': mood = NEUTRAL; surpriseUntil = 0; renderMood(); Serial.println(F("neutral")); break;
      case '1': mood = HAPPY; surpriseUntil = 0; renderMood(); Serial.println(F("happy")); break;
      case '2': mood = ANGRY; surpriseUntil = 0; renderMood(); Serial.println(F("angry")); break;
      case '3': mood = SURPRISED; surpriseUntil = 0; renderMood(); Serial.println(F("surprised")); break;
      case '4': mood = SLEEPY; surpriseUntil = 0; renderMood(); Serial.println(F("sleepy")); break;
      case 'b': case 'B': blinkNow(); break;
      case 'l': case 'L': targetX = -0.75f; break;
      case 'r': case 'R': targetX = 0.75f; break;
      case 'c': case 'C': targetX = targetY = 0; break;
      case '?': printHelp(); break;
      default: break;
    }
  }
}

void updateMPU() {
  if (!mpuReady || millis() - lastMpuRead < 25) return;
  lastMpuRead = millis();
  uint8_t raw[14];
  if (!mpuReadBytes(0x3B, raw, sizeof(raw))) return;
  int16_t ax = readI16(raw), ay = readI16(raw + 2), az = readI16(raw + 4);
  int16_t gx = readI16(raw + 8), gy = readI16(raw + 10), gz = readI16(raw + 12);
  float x = ax / 16384.0f, y = ay / 16384.0f, z = az / 16384.0f;
  float roll = atan2f(y, z) * 57.29578f;
  float pitch = atan2f(-x, sqrtf(y*y + z*z)) * 57.29578f;
  filteredRoll = filteredRoll * 0.78f + roll * 0.22f;
  filteredPitch = filteredPitch * 0.78f + pitch * 0.22f;

  // Map tilt to eye direction. Dead zone prevents jitter while resting.
  float tx = (fabsf(filteredRoll) < 7.0f) ? 0.0f : constrain(filteredRoll / 32.0f, -1.0f, 1.0f);
  float ty = (fabsf(filteredPitch) < 8.0f) ? 0.0f : constrain(filteredPitch / 32.0f, -1.0f, 1.0f);
  if (mood != SLEEPY) { targetX = tx; targetY = ty; }

  float gyroMagnitude = sqrtf((float)gx*gx + (float)gy*gy + (float)gz*gz) / 131.0f;
  float accelMagnitude = sqrtf(x*x + y*y + z*z);
  bool shake = gyroMagnitude > 230.0f || fabsf(accelMagnitude - 1.0f) > 0.55f;
  if (shake && millis() - lastShake > 1800 && mood != SLEEPY) {
    lastShake = millis();
    surpriseUntil = millis() + 900;
    Eyes::draw_open(gazeY, gazeX, 0.0f, 0.95f, 9.0f, frameBuffer);
    showBuffer();
    Serial.println(F("Shake detected!"));
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  randomSeed((uint32_t)micros());
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED not found at 0x3C. Check display wiring."));
    while (true) delay(1000);
  }
  display.clearDisplay(); display.display();
  mpuReady = initMPU();
  Eyes::draw_closed(frameBuffer); showBuffer(); delay(150);
  renderMood();
  nextBlink = millis() + random(1800, 4000);
  nextGlance = millis() + random(1200, 3000);
  printHelp();
}

void loop() {
  handleSerial();
  updateMPU();
  unsigned long now = millis();
  if (now - lastDraw >= 45) {
    lastDraw = now;
    gazeX += (targetX - gazeX) * 0.18f;
    gazeY += (targetY - gazeY) * 0.18f;
    if (mood != SLEEPY && now >= surpriseUntil) renderMood();
    else if (mood == SLEEPY) renderMood();
  }
  if (mood != SLEEPY && now >= nextGlance && now >= surpriseUntil) {
    // Small idle glances only when the sensor is close to level.
    if (mpuReady && fabsf(filteredRoll) < 8 && fabsf(filteredPitch) < 8) {
      int pick = random(4);
      targetX = (pick == 0) ? -0.45f : (pick == 1) ? 0.45f : 0.0f;
      targetY = (pick == 2) ? -0.25f : (pick == 3) ? 0.25f : 0.0f;
    } else if (!mpuReady) {
      int pick = random(4);
      targetX = (pick == 0) ? -0.55f : (pick == 1) ? 0.55f : 0.0f;
      targetY = (pick == 2) ? -0.30f : (pick == 3) ? 0.30f : 0.0f;
    }
    nextGlance = now + random(1800, 4200);
  }
  if (mood != SLEEPY && now >= nextBlink) blinkNow();
  if (surpriseUntil && now >= surpriseUntil) { surpriseUntil = 0; renderMood(); }
}
