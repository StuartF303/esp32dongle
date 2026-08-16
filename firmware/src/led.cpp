#include "led.h"

#include <Arduino.h>

namespace {

const uint8_t APA102_DATA_PIN = 40;
const uint8_t APA102_CLOCK_PIN = 39;

bool overrideActive = false;

void apa102SendByte(uint8_t b) {
  for (uint8_t bit = 0; bit < 8; bit++) {
    digitalWrite(APA102_DATA_PIN, (b & 0x80) ? HIGH : LOW);
    b <<= 1;
    digitalWrite(APA102_CLOCK_PIN, HIGH);
    digitalWrite(APA102_CLOCK_PIN, LOW);
  }
}

// Single-LED APA102 frame: 4-byte start frame, one LED frame (global
// brightness 0xE0..0xFF | 5-bit brightness, then B, G, R), 4-byte end frame
// to flush the clock.
void apa102SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 8) {
  apa102SendByte(0x00);
  apa102SendByte(0x00);
  apa102SendByte(0x00);
  apa102SendByte(0x00);

  apa102SendByte(0xE0 | (brightness & 0x1F));
  apa102SendByte(b);
  apa102SendByte(g);
  apa102SendByte(r);

  apa102SendByte(0xFF);
  apa102SendByte(0xFF);
  apa102SendByte(0xFF);
  apa102SendByte(0xFF);
}

}  // namespace

namespace Led {

void begin() {
  pinMode(APA102_DATA_PIN, OUTPUT);
  pinMode(APA102_CLOCK_PIN, OUTPUT);
  apa102SetColor(0, 0, 0);
}

void setOverrideColor(uint8_t r, uint8_t g, uint8_t b) {
  overrideActive = true;
  apa102SetColor(r, g, b);
}

void heartbeatTask() {
  static bool on = false;

  if (overrideActive) {
    return;  // console has taken manual control
  }

  on = !on;
  if (on) {
    apa102SetColor(20, 12, 0);
  } else {
    apa102SetColor(0, 0, 0);
  }
}

}  // namespace Led
