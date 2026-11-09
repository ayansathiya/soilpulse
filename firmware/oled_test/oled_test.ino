// OLED test: shows "Hello soil!" so you can check the display wiring on its own.
// Library needed: U8g2 (olikraus)
#include <U8g2lib.h>

// 1.3" OLEDs are usually SH1106. If the text is garbled or shifted sideways,
// comment this line out and use the SSD1306 line below instead.
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

void setup() {
  u8g2.begin();
  u8g2.setFont(u8g2_font_helvB14_tr);
}

void loop() {
  u8g2.clearBuffer();
  u8g2.drawStr(10, 30, "Hello soil!");
  u8g2.sendBuffer();
  delay(500);
}
