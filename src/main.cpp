#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "eyes.h"

// ESP32-WROOM-32 DevKit + I2C OLED 128x64
static constexpr int OLED_SDA = 21;
static constexpr int OLED_SCL = 18; // user-selected replacement for GPIO 22
static constexpr uint8_t OLED_ADDRESS = 0x3C; // try 0x3D if your module uses that address
static constexpr int SCREEN_WIDTH = 128;
static constexpr int SCREEN_HEIGHT = 64;
static constexpr int OLED_RESET = -1;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
uint8_t frameBuffer[SCREEN_WIDTH * SCREEN_HEIGHT / 8];

enum Mood : uint8_t { NEUTRAL, HAPPY, ANGRY, SURPRISED, SLEEPY };
Mood mood = NEUTRAL;
float gazeX = 0.0f, gazeY = 0.0f;
float targetX = 0.0f, targetY = 0.0f;
unsigned long nextBlink = 0, nextGlance = 0, lastDraw = 0;

void showBuffer() {
  display.clearDisplay();
  display.drawBitmap(0, 0, frameBuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
}

void renderMood() {
  switch (mood) {
    case HAPPY:
      Eyes::draw_open(gazeY, gazeX, 0.0f, 0.20f, 3.0f, frameBuffer); break;
    case ANGRY:
      Eyes::draw_open(gazeY, gazeX, 0.75f, 0.55f, -9.0f, frameBuffer); break;
    case SURPRISED:
      Eyes::draw_open(gazeY, gazeX, 0.0f, 0.95f, 9.0f, frameBuffer); break;
    case SLEEPY:
      Eyes::draw_half_open(frameBuffer); break;
    case NEUTRAL:
    default:
      Eyes::draw_open(gazeY, gazeX, 0.0f, 0.30f, 0.0f, frameBuffer); break;
  }
  showBuffer();
}

void blinkNow() {
  Eyes::draw_closed(frameBuffer); showBuffer();
  delay(115);
  if (mood != SLEEPY) renderMood();
  nextBlink = millis() + random(2200, 5200);
}

void printHelp() {
  Serial.println(F("\nMochi ESP32 ready"));
  Serial.println(F("0 neutral | 1 happy | 2 angry | 3 surprised | 4 sleepy"));
  Serial.println(F("b blink | l look left | r look right | c center | ? help"));
}

void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    switch (c) {
      case '0': mood = NEUTRAL; renderMood(); Serial.println(F("neutral")); break;
      case '1': mood = HAPPY; renderMood(); Serial.println(F("happy")); break;
      case '2': mood = ANGRY; renderMood(); Serial.println(F("angry")); break;
      case '3': mood = SURPRISED; renderMood(); Serial.println(F("surprised")); break;
      case '4': mood = SLEEPY; renderMood(); Serial.println(F("sleepy")); break;
      case 'b': case 'B': blinkNow(); break;
      case 'l': case 'L': targetX = -0.75f; targetY = 0.0f; break;
      case 'r': case 'R': targetX = 0.75f; targetY = 0.0f; break;
      case 'c': case 'C': targetX = 0.0f; targetY = 0.0f; break;
      case '?': printHelp(); break;
      default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  randomSeed((uint32_t)micros());
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("OLED not found. Check SDA=21, SCL=18, power and I2C address 0x3C/0x3D."));
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.display();
  Eyes::draw_closed(frameBuffer); showBuffer(); delay(180);
  renderMood();
  nextBlink = millis() + random(1800, 4000);
  nextGlance = millis() + random(1200, 3000);
  printHelp();
}

void loop() {
  handleSerial();
  const unsigned long now = millis();
  if (now - lastDraw >= 35) {
    lastDraw = now;
    gazeX += (targetX - gazeX) * 0.22f;
    gazeY += (targetY - gazeY) * 0.22f;
    if (mood != SLEEPY) renderMood();
  }
  if (mood != SLEEPY && now >= nextGlance) {
    const int pick = random(4);
    targetX = (pick == 0) ? -0.65f : (pick == 1) ? 0.65f : 0.0f;
    targetY = (pick == 2) ? -0.35f : (pick == 3) ? 0.35f : 0.0f;
    nextGlance = now + random(1600, 4200);
  }
  if (mood != SLEEPY && now >= nextBlink) blinkNow();
}
