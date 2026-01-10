#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); [cite: 7]

// --- CAT ANIMATION DRAWING ---
void drawCatFrame(int x) {
  display.clearDisplay();
  // Body
  display.fillRect(x, 15, 20, 10, SSD1306_WHITE); [cite: 15]
  // Head
  display.fillCircle(x+20, 12, 8, SSD1306_WHITE); [cite: 16]
  // Ears
  display.fillTriangle(x+14, 6, x+18, 0, x+22, 6, SSD1306_WHITE); [cite: 16]
  display.fillTriangle(x+26, 6, x+30, 0, x+24, 6, SSD1306_WHITE); [cite: 17]
  // Tail
  display.drawLine(x, 15, x-5, 5, SSD1306_WHITE); [cite: 17]
  // Legs
  if ((x / 4) % 2 == 0) { // Simple walking animation
     display.drawLine(x+2, 25, x+2, 30, SSD1306_WHITE); [cite: 18]
     display.drawLine(x+18, 25, x+18, 30, SSD1306_WHITE); [cite: 19]
  } else {
     display.drawLine(x+5, 25, x+5, 30, SSD1306_WHITE); [cite: 19]
     display.drawLine(x+15, 25, x+15, 30, SSD1306_WHITE); [cite: 20]
  }
  display.display();
}

void setup() {
  // Initialize Display (Required for animation to work)
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C); [cite: 61]

  // --- EXECUTE BOOT ANIMATION ---
  // Loops x from -30 (off-screen left) to 128 (off-screen right)
  for(int x = -30; x < 128; x+=4) {
    drawCatFrame(x); [cite: 62]
    // delay(10) is omitted to keep it smooth/fast [cite: 63]
  }
}

void loop() {
}