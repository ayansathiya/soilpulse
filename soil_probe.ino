/*
  Soil Probe - Arduino UNO R4 WiFi
  Temperature, pH and compaction (cone index), with a dashboard-style
  1.3" 128x64 OLED: page tabs on top, big reading, a plain-language verdict,
  a "Detecting..." state, and a little dancing mustache buddy.
  Also talks to the browser dashboard over USB serial.

  Libraries (Library Manager): OneWire (latest), DallasTemperature,
  HX711 (Bogdan Necula), U8g2 (olikraus)

  OLED pages:  Temp  |  pH  |  Compact
    - The screen follows the probe you are using: when the temperature jumps
      (probe goes into the soil) it shows Temp, when the pH moves it shows pH,
      and it stays there. Tune TEMP_FOCUS_DELTA / PH_FOCUS_DELTA below.
    - Button on D8 (to GND): short press = reset the reading (restarts
      "Detecting..." and measures again), hold for 1 second = next page.

  Serial (115200 baud) / dashboard commands:
    r        reading: prints "temp_C,pH" then the values
    w        raw values (temp, pH volts) for calibration
    z        tare the load cell and reset the peak (nothing on the cone)
    s<kg>    calibrate load cell with a known mass, e.g.  s2.0
    p        penetrometer mode (n = next 5 cm, x = stop)

  Wiring:
    DS18B20   red->5V, black->GND, yellow->D2, 4.7k resistor between D2 and 5V
    HX711     VCC->5V, GND->GND, DT->D3, SCK->D4 (load cell wires to E+/E-/A+/A-)
    pH board  V+->5V, G->GND, Po->A0
    OLED      VCC->3.3V, GND->GND, SDA->A4, SCL->A5
    Button    D8 to GND
*/

#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HX711.h>

// 1.3" OLEDs are almost always SH1106. If the picture is garbled or shifted
// sideways, comment this line out and use the SSD1306 line below it instead.
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---------------- Pins ----------------
const uint8_t PIN_ONEWIRE = 2;
const uint8_t PIN_HX_DT   = 3;
const uint8_t PIN_HX_SCK  = 4;
const uint8_t PIN_BTN     = 8;
const uint8_t PIN_PH      = A0;

// ---------------- Calibration constants (edit after calibrating) ----------------
float HX_SCALE = 1.0f;              // raw counts per kg  (from the 's' command)
const float CONE_AREA_M2 = 323e-6f; // 30 deg cone, 323 mm^2 base. Measure YOUR cone!

float PH_V7 = 2.50f;                // volts at pH 7  (from the 'w' command)
float PH_V4 = 3.00f;                // volts at pH 4
const float PH_CAL_TEMP_C = 25.0f;  // temperature your buffers were at

const uint32_t AUTO_CYCLE_MS = 0;    // 0 = never auto-switch (screen follows the active probe)
const float TEMP_FOCUS_DELTA = 1.0f; // temp change (C) that pulls the screen to the Temp page
const float PH_FOCUS_DELTA   = 0.4f; // pH change that pulls the screen to the pH page

// ---------------- State ----------------
const float ADC_MAX = 16383.0f;     // 14-bit ADC
const float VREF    = 5.0f;

OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);
HX711 scale;
bool hxOK = false;

float coneKPa = 0, peakKPa = 0;
float tempC = NAN, phVal = NAN, prevTemp = NAN;
float tempAnchor = NAN, phAnchor = NAN;   // last values that moved the screen focus
uint32_t graceUntil = 0;                  // ignore focus changes right after boot
uint8_t phStableCount = 0, tempStableCount = 0;

uint8_t page = 0;                   // 0 = Temp, 1 = pH, 2 = Compact
uint32_t lastPageChange = 0, autoUntil = 0, flashUntil = 0;
const char* flashMsg = "";
bool penActive = false;

// ---------------- Sensors ----------------
float readVolts(uint8_t pin, int n = 32) {
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return (sum / (float)n) / ADC_MAX * VREF;
}

// pH with simple Nernst temperature compensation of the slope
float readPH(float tempC) {
  float v = readVolts(PIN_PH);
  float voltsPerPH = (PH_V4 - PH_V7) / (4.0f - 7.0f);               // at calibration temp
  float scaleT = (tempC + 273.15f) / (PH_CAL_TEMP_C + 273.15f);
  return 7.0f + (v - PH_V7) / (voltsPerPH * scaleT);
}

// Throw out impossible DS18B20 values instead of showing them:
//  -127 = sensor not found / bad data wire, 85.0 = not finished measuring yet
float cleanTemp(float t) {
  if (t == DEVICE_DISCONNECTED_C) return NAN;
  if (t < -55.0f || t > 125.0f) return NAN;
  if (t == 85.0f) return NAN;
  return t;
}

// The screen follows the probe you are using: a big change in a reading means
// you just moved that probe, so show its page and stay there.
void noteActivity(float cur, float &anchor, float delta, uint8_t pageIdx) {
  if (isnan(cur)) return;
  if (isnan(anchor)) { anchor = cur; return; }
  if (fabs(cur - anchor) >= delta) {
    anchor = cur;
    if (millis() > graceUntil && !penActive && page != pageIdx) {
      page = pageIdx;
      lastPageChange = millis();
    }
  }
}

// Fresh temperature (waits for the conversion), then back to non-blocking mode
float readTempBlocking() {
  sensors.setWaitForConversion(true);
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  return cleanTemp(t);
}

// Non-blocking: only reads the HX711 when a new sample is ready (10 per second).
// Returns true when a new sample was taken.
bool updateCone() {
  if (!hxOK || !scale.is_ready()) return false;
  float newtons = fabs(scale.get_units(1)) * 9.80665f;
  float kPa = newtons / CONE_AREA_M2 / 1000.0f;
  coneKPa = 0.6f * coneKPa + 0.4f * kPa;          // light smoothing
  if (coneKPa > peakKPa) peakKPa = coneKPa;
  return true;
}

// Once a second: temperature, pH, and whether each has settled
uint32_t lastSlow = 0;
void updateSlow() {
  if (millis() - lastSlow < 1000) return;
  lastSlow = millis();

  float t = sensors.getTempCByIndex(0);   // result of the previous request
  tempC = cleanTemp(t);
  if (isnan(tempC)) {                     // not found: look for it again (plug-in after boot)
    sensors.begin();
    sensors.setWaitForConversion(false);
  }
  sensors.requestTemperatures();          // start the next one

  bool tSteady = !isnan(tempC) && !isnan(prevTemp) && fabs(tempC - prevTemp) < 0.3f;
  tempStableCount = tSteady ? (tempStableCount < 9 ? tempStableCount + 1 : 9) : 0;
  prevTemp = tempC;

  float newPH = readPH(isnan(tempC) ? 25.0f : tempC);
  if (newPH < 0.0f || newPH > 14.0f) newPH = NAN;   // impossible: probe not in liquid / not calibrated
  bool pSteady = !isnan(phVal) && !isnan(newPH) && fabs(newPH - phVal) < 0.03f;
  phStableCount = pSteady ? (phStableCount < 9 ? phStableCount + 1 : 9) : 0;
  phVal = newPH;

  noteActivity(tempC, tempAnchor, TEMP_FOCUS_DELTA, 0);
  noteActivity(phVal, phAnchor, PH_FOCUS_DELTA, 1);
}

// ---------------- Plain-language verdicts (max 25 characters) ----------------
const char* tempVerdict(float t) {
  if (t < 5)   return "Too cold for seeds";
  if (t < 10)  return "Cool, slow germination";
  if (t < 26)  return "Good for seeds & roots";
  return "Warm, watch moisture";
}
const char* phVerdict(float p) {
  if (p < 5.5f) return "Strongly acidic";
  if (p < 6.0f) return "Acidic, tough for most";
  if (p < 7.1f) return "Good for most plants";
  if (p < 7.9f) return "Slightly alkaline";
  return "Alkaline, hard to feed";
}
const char* coneVerdict(float k) {
  if (k < 1000) return "Loose, roots move free";
  if (k < 2000) return "Firm, roots slowed";
  return "Compacted, roots stuck";
}

// ---------------- OLED drawing ----------------
// The dancing mustache buddy. cx = centre x, baseY = ground line.
void drawMascot(int cx, int baseY, bool blinkNow) {
  uint8_t phase = (millis() / 250) % 4;          // 4-step dance, 1 second per cycle
  int hop = (phase & 1) ? 2 : 0;
  int sx  = (phase < 2) ? -1 : 1;                // sway left / right
  int bx  = cx + sx;
  int by  = baseY - 14 - hop;                    // body centre

  u8g2.setDrawColor(1);
  // feet (alternate lifting)
  u8g2.drawFilledEllipse(cx - 6, baseY - 2 - (phase < 2 ? 2 : 0), 5, 2);
  u8g2.drawFilledEllipse(cx + 6, baseY - 2 - (phase >= 2 ? 2 : 0), 5, 2);
  // body
  u8g2.drawFilledEllipse(bx, by, 12, 11);
  // arms (swing opposite to each other)
  u8g2.drawFilledEllipse(bx - 14, by + (phase < 2 ? -4 : 3), 2, 4);
  u8g2.drawFilledEllipse(bx + 14, by + (phase < 2 ? 3 : -4), 2, 4);
  // sprout on top
  u8g2.drawVLine(bx, by - 15, 5);
  u8g2.drawDisc(bx - 3 + sx, by - 16, 2);
  u8g2.drawDisc(bx + 3 + sx, by - 17, 2);

  // face, drawn in "off" pixels on the lit body
  u8g2.setDrawColor(0);
  u8g2.drawLine(bx - 8, by - 6, bx - 3, by - 7);
  u8g2.drawLine(bx + 3, by - 7, bx + 8, by - 6);
  if (blinkNow) {
    u8g2.drawHLine(bx - 7, by - 2, 4);
    u8g2.drawHLine(bx + 3, by - 2, 4);
  } else {
    u8g2.drawDisc(bx - 5, by - 2, 2);
    u8g2.drawDisc(bx + 5, by - 2, 2);
  }
  u8g2.drawFilledEllipse(bx - 5, by + 5, 5, 3);  // mustache
  u8g2.drawFilledEllipse(bx + 5, by + 5, 5, 3);
  u8g2.setDrawColor(1);
  if (!blinkNow) {                               // eye shine
    u8g2.drawPixel(bx - 6, by - 3);
    u8g2.drawPixel(bx + 4, by - 3);
  }

  // a little music note drifting up
  int ny = 30 - (int)((millis() / 80) % 16);
  u8g2.drawDisc(cx + 17, ny, 1);
  u8g2.drawVLine(cx + 18, ny - 5, 5);
}

void drawTabs() {
  const char* names[3] = { "Temp", "pH", "Compact" };
  u8g2.setFont(u8g2_font_6x10_tr);
  int x = 0;
  for (int i = 0; i < 3; i++) {
    int w = u8g2.getStrWidth(names[i]) + 8;
    if (i == page) {                              // active tab: filled pill
      u8g2.setDrawColor(1);
      u8g2.drawRBox(x, 0, w, 12, 3);
      u8g2.setDrawColor(0);
      u8g2.drawStr(x + 4, 9, names[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(x + 4, 9, names[i]);
    }
    x += w + 3;
  }
}

void drawFrame() {
  String val = "--";
  const char* unit = "";
  const char* caption = "";
  const char* verdict = "";
  bool detecting = false;

  if (page == 0) {
    caption = "Soil temperature";
    unit = "C";
    if (isnan(tempC)) {
      verdict = "No temp probe";
    } else {
      val = String(tempC, 1);
      verdict = tempVerdict(tempC);
      detecting = tempStableCount < 2;
    }
  } else if (page == 1) {
    caption = "pH of soil slurry";
    if (isnan(phVal)) {
      verdict = "Check probe / calibrate";
    } else {
      val = String(phVal, 2);
      verdict = phVerdict(phVal);
      detecting = phStableCount < 2;
    }
  } else {
    caption = "Peak cone index";
    unit = "kPa";
    if (!hxOK) {
      verdict = "No load cell";
    } else {
      val = String(peakKPa, 0);
      if (peakKPa < 30)                 verdict = "Push the cone in";
      else if (coneKPa > 0.9f * peakKPa) detecting = true;   // still pushing
      else                               verdict = coneVerdict(peakKPa);
    }
  }

  u8g2.clearBuffer();
  drawTabs();

  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 20, caption);

  u8g2.setFont(u8g2_font_logisoso24_tn);
  u8g2.drawStr(0, 47, val.c_str());
  int w = u8g2.getStrWidth(val.c_str());
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(w + 4, 47, unit);

  u8g2.setFont(u8g2_font_5x8_tr);
  String line = verdict;
  if (millis() < flashUntil) {
    line = flashMsg;
  } else if (detecting) {
    line = "Detecting";
    for (uint8_t i = 0; i < (millis() / 400) % 4; i++) line += ".";
  }
  u8g2.drawStr(0, 61, line.c_str());

  drawMascot(104, 51, (millis() % 3200) < 140);
  u8g2.sendBuffer();
}

// ---------------- Button, paging ----------------
void tareAndReset() {
  if (hxOK) scale.tare(20);
  peakKPa = 0;
  coneKPa = 0;
}

void nextPage(bool byButton) {
  page = (page + 1) % 3;
  lastPageChange = millis();
  if (byButton) autoUntil = millis() + 20000;     // pause auto-switching for 20 s
}

// Throw away the current reading and measure again from scratch
void resetReading() {
  phStableCount = 0;
  tempStableCount = 0;
  prevTemp = NAN;
  phVal = NAN;
  tempAnchor = NAN;
  phAnchor = NAN;
  peakKPa = 0;
  coneKPa = 0;
  lastSlow = 0;                                    // measure again right away
  autoUntil = millis() + 20000;                    // stay on this page for a while
  flashMsg = "Reading reset";
  flashUntil = millis() + 1200;
}

void handleButton() {
  static bool down = false, longDone = false;
  static uint32_t t0 = 0;
  bool pressed = (digitalRead(PIN_BTN) == LOW);
  if (pressed && !down) {
    down = true; longDone = false; t0 = millis();
  } else if (pressed && down && !longDone && millis() - t0 >= 1000) {
    longDone = true;
    nextPage(true);
  } else if (!pressed && down) {
    down = false;
    if (!longDone && millis() - t0 > 30) resetReading();
  }
}

// One pass of everything the display needs. Returns true on a new load-cell sample.
bool tick() {
  bool fresh = updateCone();
  updateSlow();
  handleButton();

  if (AUTO_CYCLE_MS && !penActive && millis() > autoUntil &&
      millis() - lastPageChange > AUTO_CYCLE_MS && !(page == 2 && coneKPa > 30)) {
    nextPage(false);
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 80) {
    lastDraw = millis();
    drawFrame();
  }
  return fresh;
}

// ---------------- Serial commands ----------------
void takeReading() {
  float t = readTempBlocking();
  float ph = readPH(isnan(t) ? 25.0f : t);
  Serial.println(F("temp_C,pH"));
  Serial.print(t, 2);  Serial.print(',');
  Serial.println(ph, 2);
}

void printRaw() {
  Serial.print(F("Temp C: "));        Serial.println(readTempBlocking(), 2);
  Serial.print(F("pH pin volts: "));  Serial.println(readVolts(PIN_PH, 64), 3);
}

void calibrateScale() {
  float kg = Serial.parseFloat();   // e.g. "s2.0"
  if (kg <= 0) {
    Serial.println(F("Usage: s<known mass in kg>, e.g. s2.0  (tare with 'z' first)"));
    return;
  }
  scale.set_scale();                // reset to raw counts
  long raw = scale.get_value(20);   // raw minus tare offset
  float factor = raw / kg;
  scale.set_scale(factor);
  Serial.print(F("HX_SCALE = "));
  Serial.println(factor, 2);
  Serial.println(F("Paste this into HX_SCALE at the top of the sketch."));
}

void penetrometer() {
  Serial.println(F("depth_cm,force_N,cone_index_kPa   (n = next 5 cm, x = stop)"));
  peakKPa = 0;
  int depthCm = 0;
  penActive = true;
  page = 2;
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'x') break;
      if (c == 'n') depthCm += 5;
    }
    if (tick()) {                                  // new sample, about 10 per second
      float newtons = coneKPa * CONE_AREA_M2 * 1000.0f;
      Serial.print(depthCm);     Serial.print(',');
      Serial.print(newtons, 1);  Serial.print(',');
      Serial.println(coneKPa, 0);
    }
  }
  penActive = false;
  lastPageChange = millis();
  Serial.println(F("Penetrometer stopped."));
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  analogReadResolution(14);
  pinMode(PIN_BTN, INPUT_PULLUP);

  u8g2.begin();
  u8g2.setBusClock(400000);                        // faster I2C = smoother dancing

  uint32_t t0 = millis();                          // dancing splash screen
  while (millis() - t0 < 1800) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB12_tr);
    u8g2.drawStr(0, 28, "Soil Probe");
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 44, "warming up...");
    drawMascot(104, 51, false);
    u8g2.sendBuffer();
    delay(60);
  }

  sensors.begin();
  sensors.requestTemperatures();                   // first conversion (blocking)
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  hxOK = scale.wait_ready_timeout(1000);
  if (hxOK) {
    scale.set_scale(HX_SCALE);
    scale.tare(20);
  } else {
    Serial.println(F("HX711 not found - check DT/SCK wiring."));
  }

  lastPageChange = millis();
  graceUntil = millis() + 6000;                    // let readings settle before the screen follows probes
  Serial.println(F("Soil probe ready. Commands: r w z s<kg> p"));
}

void loop() {
  tick();

  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 'r': takeReading(); break;
    case 'w': printRaw(); break;
    case 'z': tareAndReset(); Serial.println(F("Tared.")); break;
    case 's': calibrateScale(); break;
    case 'p': penetrometer(); break;
    default: break;
  }
}
