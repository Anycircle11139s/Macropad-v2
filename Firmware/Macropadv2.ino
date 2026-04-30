#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DRV2605.h>
#include <FastLED.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

// ==========================================
// 1. HARDWARE CONFIGURATION (VERIFY THESE!)
// ==========================================

// --- I2C Multiplexer (TCA9548A) ---
#define TCAADDR 0x70
#define OLED_1_CH 0 // Channel for Left OLED
#define OLED_2_CH 1 // Channel for Right OLED

// --- Switch Matrix (via MCP23017) ---
const int NUM_ROWS = 3;
const int NUM_COLS = 4;
// MAP THESE TO YOUR MCP23017 PINS (0-7 = GPA, 8-15 = GPB)
const uint8_t rowPins[NUM_ROWS] = {0, 1, 2};    // Example: GPA0, GPA1, GPA2
const uint8_t colPins[NUM_COLS] = {8, 9, 10, 11}; // Example: GPB0, GPB1, GPB2, GPB3

// Keymap matching your 3x4 grid
const char keyMap[NUM_ROWS][NUM_COLS] = {
  {'1', '2', '3', '4'},
  {'Q', 'W', 'E', 'R'},
  {'A', 'S', 'D', 'F'}
};
bool keyStates[NUM_ROWS][NUM_COLS] = {false};

// --- Trackball (Assuming GPIO direct to ESP32) ---
// If your trackball is I2C, this logic changes. Assuming 4 hall sensors here.
#define TB_UP    D1 
#define TB_DOWN  D2
#define TB_LEFT  D3
#define TB_RIGHT D4
#define TB_SPEED 2

// --- RGB LEDs (SK6812) ---
#define LED_PIN     D0 // "led data 1" pin
#define NUM_LEDS    10 // Adjust to your actual LED count
CRGB leds[NUM_LEDS];

// ==========================================
// 2. GLOBAL OBJECTS
// ==========================================

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;

Adafruit_SSD1306 display1(128, 32, &Wire, -1);
Adafruit_SSD1306 display2(128, 32, &Wire, -1);
Adafruit_ADS1115 ads;
Adafruit_MCP23X17 mcp;
Adafruit_DRV2605 drv;

unsigned long lastOLEDUpdate = 0;

// ==========================================
// 3. HELPER FUNCTIONS
// ==========================================

// Switch I2C Multiplexer Channel
void tcaselect(uint8_t i) {
  if (i > 7) return;
  Wire.beginTransmission(TCAADDR);
  Wire.write(1 << i);
  Wire.endTransmission();
}

// Trigger Haptic Feedback
void playHapticClick() {
  drv.setWaveform(0, 1);  // 1 = Strong Click (from DRV2605 library)
  drv.setWaveform(1, 0);  // End sequence
  drv.go();
}

// ==========================================
// 4. SETUP
// ==========================================

void setup() {
  Serial.begin(115200);
  
  // Start USB HID
  USB.begin();
  Keyboard.begin();
  Mouse.begin();

  // Start I2C
  Wire.begin();
  Wire.setClock(400000); // 400kHz for snappy matrix scanning

  // Init LEDs
  FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(60);
  fill_solid(leds, NUM_LEDS, CRGB::Cyan);
  FastLED.show();

  // Init MCP23017 Matrix
  if (!mcp.begin_I2C()) {
    Serial.println("MCP23017 missing.");
  } else {
    // Set columns as INPUT_PULLUP
    for (int c = 0; c < NUM_COLS; c++) {
      mcp.pinMode(colPins[c], INPUT_PULLUP);
    }
    // Set rows as OUTPUT and HIGH
    for (int r = 0; r < NUM_ROWS; r++) {
      mcp.pinMode(rowPins[r], OUTPUT);
      mcp.digitalWrite(rowPins[r], HIGH);
    }
  }

  // Init ADS1115
  ads.setGain(GAIN_ONE); // +/- 4.096V
  if (!ads.begin()) { Serial.println("ADS1115 missing."); }

  // Init Haptics
  if (!drv.begin()) {
    Serial.println("DRV2605 missing.");
  } else {
    drv.selectLibrary(1);
    drv.setMode(DRV2605_MODE_INTTRIG);
  }

  // Init OLED 1
  tcaselect(OLED_1_CH);
  if(display1.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display1.clearDisplay();
    display1.setTextColor(SSD1306_WHITE);
    display1.setCursor(10, 10);
    display1.print("SYSTEM READY");
    display1.display();
  }

  // Init Trackball Pins
  pinMode(TB_UP, INPUT_PULLUP);
  pinMode(TB_DOWN, INPUT_PULLUP);
  pinMode(TB_LEFT, INPUT_PULLUP);
  pinMode(TB_RIGHT, INPUT_PULLUP);
}

// ==========================================
// 5. MAIN LOOP
// ==========================================

void loop() {
  
  // --- A. SCAN SWITCH MATRIX ---
  for (int r = 0; r < NUM_ROWS; r++) {
    // Pull the current row LOW
    mcp.digitalWrite(rowPins[r], LOW);
    
    for (int c = 0; c < NUM_COLS; c++) {
      // Read the column. LOW means pressed (because of pullups)
      bool isPressed = !mcp.digitalRead(colPins[c]);
      
      // State change detection
      if (isPressed && !keyStates[r][c]) {
        keyStates[r][c] = true;
        Keyboard.press(keyMap[r][c]);
        playHapticClick();
        
        // Visual feedback
        leds[0] = CRGB::White; 
        FastLED.show();
      } 
      else if (!isPressed && keyStates[r][c]) {
        keyStates[r][c] = false;
        Keyboard.release(keyMap[r][c]);
        
        leds[0] = CRGB::Cyan; 
        FastLED.show();
      }
    }
    // Return row to HIGH before moving to next
    mcp.digitalWrite(rowPins[r], HIGH);
  }


  // --- B. READ ANALOG JOYSTICK ---
  // ADC range for GAIN_ONE is roughly 0 to 26600 for 3.3V
  int16_t joyX = ads.readADC_SingleEnded(0); 
  int16_t joyY = ads.readADC_SingleEnded(1); 
  
  // Define Center deadzone (Assumes center is ~13300)
  int deadzone = 2000;
  int center = 13300;
  int mouseX = 0;
  int mouseY = 0;

  if (joyX > (center + deadzone)) mouseX = map(joyX, center + deadzone, 26600, 1, 10);
  if (joyX < (center - deadzone)) mouseX = map(joyX, center - deadzone, 0, -1, -10);
  
  if (joyY > (center + deadzone)) mouseY = map(joyY, center + deadzone, 26600, 1, 10);
  if (joyY < (center - deadzone)) mouseY = map(joyY, center - deadzone, 0, -1, -10);

  if (mouseX != 0 || mouseY != 0) {
    Mouse.move(mouseX, mouseY);
  }


  // --- C. READ TRACKBALL ---
  if (!digitalRead(TB_UP))    Mouse.move(0, -TB_SPEED);
  if (!digitalRead(TB_DOWN))  Mouse.move(0, TB_SPEED);
  if (!digitalRead(TB_LEFT))  Mouse.move(-TB_SPEED, 0);
  if (!digitalRead(TB_RIGHT)) Mouse.move(TB_SPEED, 0);


  // --- D. UPDATE OLEDS (Non-Blocking) ---
  // Only update screens every 100ms to prevent lagging the matrix scanner
  if (millis() - lastOLEDUpdate > 100) {
    tcaselect(OLED_1_CH);
    display1.clearDisplay();
    display1.setCursor(0,0);
    display1.print("X: "); display1.println(joyX);
    display1.print("Y: "); display1.println(joyY);
    display1.display();

    lastOLEDUpdate = millis();
  }

  // Breathing room for I2C bus stability
  delay(5); 
}
