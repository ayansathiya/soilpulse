/*
  Soil Probe - Arduino UNO R4 WiFi
  Measures soil/slurry temperature, pH and compaction (cone index).
  Shows live values on a 1.3" 128x64 OLED and talks to the dashboard over USB.

  Libraries (Library Manager): OneWire (latest), DallasTemperature,
  HX711 (Bogdan Necula), U8g2 (olikraus)

  Serial Monitor / dashboard: 115200 baud. Commands:
    r        reading: prints "temp_C,pH" then the values
    w        raw values (temp, pH volts) for calibration
    z        tare the load cell and reset the peak (nothing on the cone)
    s<kg>    calibrate load cell with a known mass, e.g.  s2.0
    p        penetrometer mode (n = next 5 cm, x = stop)

  Wiring:
    DS18B20   red->5V, black->GND, yellow->D2, 4.7k resistor between D2 and 5V
    HX711     VCC->5V, GND->GND, DT->D3, SCK->D4 (load cell wires to E+/E-/A+/A-)
    pH board  V+->5V, G->GND, Po->A0 (glass probe plugs into the board's BNC)
    OLED      VCC->3.3V, GND->GND, SDA->A4, SCL->A5
    Button    (optional) D7 to GND: tares the load cell and resets the peak
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
const uint8_t PIN_BTN     = 7;
const uint8_t PIN_PH      = A0;

// ---------------- Calibration constants (edit after calibrating) ----------------
float HX_SCALE = 1.0f;              // raw counts per kg  (from the 's' command)
const float CONE_AREA_M2 = 323e-6f; // 30 deg cone, 323 mm^2 base. Measure YOUR cone!

float PH_V7 = 2.50f;                // volts at pH 7  (from the 'w' command)
float PH_V4 = 3.00f;                // volts at pH 4
const float PH_CAL_TEMP_C = 25.0f;  // temperature your buffers were at

// ---------------- State ----------------
const float ADC_MAX = 16383.0f;     // 14-bit ADC
const float VREF    = 5.0f;

OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);
HX711 scale;
bool hxOK = false;
float peakKPa = 0;

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

// Fresh temperature (waits for the conversion), then back to non-blocking mode
float readTempBlocking() {
  sensors.setWaitForConversion(true);
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  return (t == DEVICE_DISCONNECTED_C) ? NAN : t;
}

float readConeKPa() {
  if (!hxOK) return NAN;
  float newtons = fabs(scale.get_units(1)) * 9.80665f;
  float kPa = newtons / CONE_AREA_M2 / 1000.0f;
  if (kPa > peakKPa) peakKPa = kPa;
  return kPa;
}

// ---------------- OLED ----------------
void drawScreen(float t, float ph, float peak) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 16, "Temp");
  u8g2.drawStr(0, 37, "pH");
  u8g2.drawStr(0, 58, "Peak");

  u8g2.setFont(u8g2_font_helvB14_tr);
  String a = isnan(t)  ? String("--") : String(t, 1) + " C";
  String b = isnan(ph) ? String("--") : String(ph, 2);
  String c = String(peak, 0) + " kPa";
  u8g2.drawStr(44, 16, a.c_str());
  u8g2.drawStr(44, 37, b.c_str());
  u8g2.drawStr(44, 58, c.c_str());
  u8g2.sendBuffer();
}

// Refresh about once a second (the DS18B20 needs ~750 ms per conversion)
void refreshDisplay() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 1000) return;
  lastMs = millis();

  float t = sensors.getTempCByIndex(0);   // result of the previous request
  sensors.requestTemperatures();          // start the next one
  if (t == DEVICE_DISCONNECTED_C) t = NAN;

  readConeKPa();
  float ph = readPH(isnan(t) ? 25.0f : t);
  drawScreen(t, ph, peakKPa);
}

// ---------------- Commands ----------------
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

void tareAndReset() {
  if (hxOK) scale.tare(20);
  peakKPa = 0;
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
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'x') break;
      if (c == 'n') depthCm += 5;
    }
    float kPa = readConeKPa();
    float newtons = kPa * CONE_AREA_M2 * 1000.0f;
    Serial.print(depthCm);       Serial.print(',');
    Serial.print(newtons, 1);    Serial.print(',');
    Serial.println(kPa, 0);
    refreshDisplay();
  }
  Serial.println(F("Penetrometer stopped."));
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  analogReadResolution(14);
  pinMode(PIN_BTN, INPUT_PULLUP);

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(12, 28, "Soil Probe");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(12, 46, "warming up...");
  u8g2.sendBuffer();

  sensors.begin();
  sensors.requestTemperatures();          // first conversion (blocking)
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

  Serial.println(F("Soil probe ready. Commands: r w z s<kg> p"));
}

void loop() {
  refreshDisplay();

  if (digitalRead(PIN_BTN) == LOW) {      // optional button on D7
    delay(30);
    if (digitalRead(PIN_BTN) == LOW) {
      tareAndReset();
      while (digitalRead(PIN_BTN) == LOW) {}
    }
  }

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
