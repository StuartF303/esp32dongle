#include "led.h"

#include <Arduino.h>

namespace {

const uint8_t APA102_DATA_PIN = 40;
const uint8_t APA102_CLOCK_PIN = 39;

bool overrideActive_ = false;
bool blinkOn_ = false;
uint8_t lastR_ = 0, lastG_ = 0, lastB_ = 0;

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

  lastR_ = r;
  lastG_ = g;
  lastB_ = b;
}

}  // namespace

namespace Led {

void begin() {
  pinMode(APA102_DATA_PIN, OUTPUT);
  pinMode(APA102_CLOCK_PIN, OUTPUT);
  apa102SetColor(0, 0, 0);
}

void setOverrideColor(uint8_t r, uint8_t g, uint8_t b) {
  overrideActive_ = true;
  apa102SetColor(r, g, b);
}

void clearOverride() {
  overrideActive_ = false;
  blinkOn_ = false;
  apa102SetColor(0, 0, 0);
}

bool overrideActive() { return overrideActive_; }

void off() {
  blinkOn_ = false;
  apa102SetColor(0, 0, 0);
}

void currentColor(uint8_t *r, uint8_t *g, uint8_t *b) {
  *r = lastR_;
  *g = lastG_;
  *b = lastB_;
}

void heartbeatTask() {
  if (overrideActive_) {
    return;  // manual colour set; leave it alone
  }

  blinkOn_ = !blinkOn_;
  if (blinkOn_) {
    apa102SetColor(20, 12, 0);
  } else {
    apa102SetColor(0, 0, 0);
  }
}

}  // namespace Led
