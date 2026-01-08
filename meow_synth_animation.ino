#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C // Common I2C address

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 1. THE CAT BITMAP (16x16 pixels)
// This is a simple pixel-art cat face.
const unsigned char PROGMEM catBitmap[] = {
  0x00, 0x00, 0x0C, 0x30, 0x1E, 0x78, 0x33, 0xCC, 
  0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 
  0x7F, 0xFE, 0x3F, 0xFC, 0x3F, 0xFC, 0x13, 0xC8, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

int catX = -16; // Start off-screen to the left
int catY = 24;  // Vertical position (middle of screen)

void setup() {
  Serial.begin(115200);

  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Meow Synth Active");
  display.display();
  delay(1000);
}

void loop() {
  display.clearDisplay();

  // Draw the Title
  display.setCursor(20, 0);
  display.println("MEOW SYNTH");

  // 2. DRAW THE CAT
  // drawBitmap(x, y, bitmap_data, width, height, color)
  display.drawBitmap(catX, catY, catBitmap, 16, 16, SSD1306_WHITE);

  display.display();

  // 3. ANIMATION LOGIC (Slide right)
  catX = catX + 2; // Move 2 pixels to the right

  // If cat goes completely off the right edge, reset to left
  if (catX > SCREEN_WIDTH) {
    catX = -16; 
  }

  delay(50); // Control animation speed
}