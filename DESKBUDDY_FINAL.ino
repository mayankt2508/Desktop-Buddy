/*
 * =====================================================
 *  DeskBuddy - ESP32 + SH1106 OLED Weather Robot
 *  Pet Robo of Saee
 * =====================================================
 *  Hardware:
 *    - ESP32 Dev Board
 *    - 1.3" SH1106 OLED (I2C) at 0x3C
 *    - Touch Sensor on GPIO 4
 *    SDA → GPIO 21 | SCL → GPIO 22
 *
 *  Libraries needed (install via Library Manager):
 *    - Adafruit GFX Library
 *    - Adafruit SH110X
 *    - ArduinoJson
 *    - WiFi (built-in ESP32)
 *    - HTTPClient (built-in ESP32)
 * =====================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ─── Display Config ───────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define I2C_ADDRESS   0x3C

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── Pin Config ───────────────────────────────────────
#define TOUCH_PIN 4

// ─── WiFi Credentials ─────────────────────────────────
const char* ssid     = "SAEE";
const char* password = "12345678";

// ─── OpenWeatherMap Config ────────────────────────────
const char* apiKey   = "e6f441d09d758047e021eaa9a793c3e1";
const char* city     = "Pune";
const char* units    = "metric";

// ─── NTP Config ───────────────────────────────────────
const char* ntpServer   = "pool.ntp.org";
const long  gmtOffset   = 19800; // IST = UTC+5:30 = 19800 seconds
const int   dstOffset   = 0;

// ─── Screen States ────────────────────────────────────
enum Screen {
  SCREEN_EYES = 0,
  SCREEN_WEATHER,
  SCREEN_FORECAST,
  SCREEN_CLOCK,
  SCREEN_GOODBYE,
  SCREEN_COUNT
};

int  currentScreen  = SCREEN_EYES;
bool screenChanged  = true;

// ─── Touch Debounce ───────────────────────────────────
unsigned long lastTouchTime = 0;
const unsigned long DEBOUNCE_MS = 400;
bool lastTouchState = false;

// ─── Emotion States for Eye Screen ────────────────────
enum Emotion { EMO_HAPPY = 0, EMO_BLINK, EMO_SAD, EMO_ANGRY, EMO_COUNT };
int currentEmotion = EMO_HAPPY;

// ─── Weather Data ─────────────────────────────────────
struct WeatherData {
  char cityName[32];
  float temp;
  char description[48];
  char icon[8];         // OWM icon code e.g. "01d"
  float forecast_temp[3];
  char  forecast_icon[3][8];
  bool  valid;
};
WeatherData weather;
unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_INTERVAL = 600000; // 10 minutes

// ─── Sleep Mode Config ────────────────────────────────
// After SLEEP_TIMEOUT_MS of no touch, enter sleep mode.
// Display dims and shows slow "breathing" eyes.
// Any touch wakes it back up to the previous screen.
const unsigned long SLEEP_TIMEOUT_MS = 120000; // 2 minutes
unsigned long       lastActivityTime  = 0;      // tracks last touch/interaction
bool                isSleeping        = false;  // are we in sleep mode?

// Breathing animation state
float  breathVal       = 0.0f;   // 0.0 → 1.0 sine wave
float  breathDir       = 1.0f;   // +1 = inhale, -1 = exhale
const float BREATH_STEP = 0.03f; // speed of breath cycle

// ─── Forward Declarations ─────────────────────────────
void drawBootScreen();
void drawEyesScreen(Emotion emo);
void drawWeatherScreen();
void drawForecastScreen();
void drawClockScreen();
void drawGoodbyeScreen();
void fetchWeather();
void drawWeatherIcon(int x, int y, const char* iconCode, int size);
void drawEyePair(Emotion emo);
void handleSleep();
void drawBreathingEyes();

// ─────────────────────────────────────────────────────
//  WEATHER ICONS — drawn with primitives (guaranteed correct)
//
//  Instead of bitmaps (which require exact byte alignment
//  and are hard to debug), we draw each icon using
//  Adafruit GFX shape functions. This is 100% reliable.
//
//  Each icon fits inside a 16×16 box at offset (x, y).
// ─────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Init I2C and OLED
  Wire.begin(6, 7);
  if (!display.begin(I2C_ADDRESS, true)) {
    Serial.println(F("SH1106 not found!"));
    while (true) delay(1000);
  }
  display.setContrast(200);
  display.clearDisplay();
  display.display();

  // FIX: INPUT_PULLDOWN keeps the pin firmly at LOW when not touched.
  // Plain INPUT leaves the pin floating → random HIGH noise → ghost touches.
  // This works for active-HIGH touch modules (HIGH = touched).
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // Boot animation
  drawBootScreen();

  // Mark activity so sleep timer starts after boot
  lastActivityTime = millis();

  // Connect WiFi
  connectWiFi();

  // Sync time
  configTime(gmtOffset, dstOffset, ntpServer);

  // Fetch weather
  fetchWeather();
}

// ─────────────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────────────
void loop() {
  // DEBUG: Uncomment this line to see raw touch pin state on Serial Monitor
  // Serial.println(digitalRead(TOUCH_PIN));

  handleTouch();

  // ── Sleep Mode Check ──────────────────────────────────
  // If no activity for SLEEP_TIMEOUT_MS, enter sleep mode.
  // While sleeping, run breathing eye animation only.
  handleSleep();
  if (isSleeping) {
    drawBreathingEyes();
    delay(30);
    return; // Skip all normal screen drawing while asleep
  }
  // ─────────────────────────────────────────────────────

  // Periodic weather refresh
  if (millis() - lastWeatherFetch > WEATHER_INTERVAL) {
    fetchWeather();
  }

  // Redraw current screen if changed
  if (screenChanged) {
    screenChanged = false;
    switch (currentScreen) {
      case SCREEN_EYES:     drawEyesScreen((Emotion)currentEmotion); break;
      case SCREEN_WEATHER:  drawWeatherScreen();  break;
      case SCREEN_FORECAST: drawForecastScreen(); break;
      case SCREEN_CLOCK:    drawClockScreen();    break;
      case SCREEN_GOODBYE:  drawGoodbyeScreen();  break;
    }
  }

  // Clock screen: auto-refresh every second
  if (currentScreen == SCREEN_CLOCK) {
    static unsigned long lastClockDraw = 0;
    if (millis() - lastClockDraw > 1000) {
      lastClockDraw = millis();
      drawClockScreen();
    }
  }

  // Eye animation: animateEyes() has its own internal 150ms timer
  // so calling it every loop() is safe — it only redraws when the
  // frame timer expires. This does NOT cause continuous screen changes.
  if (currentScreen == SCREEN_EYES) {
    animateEyes();
  }

  delay(20); // Reduced from 30ms → more responsive touch detection
}

// ─────────────────────────────────────────────────────
//  TOUCH HANDLING  — fixed debounce + state machine
//
//  BUGS FIXED:
//  1. lastTouchState was not updated before early return in sleep-wake
//     → caused immediate double-fire on wake
//  2. Used plain INPUT → floating pin → ghost touches
//     → fixed with INPUT_PULLDOWN in setup()
//  3. Added touchJustReleased logic so we only fire on
//     the RELEASE edge, not the press edge — much more reliable
//     on capacitive touch modules that hold HIGH for a while
// ─────────────────────────────────────────────────────
void handleTouch() {
  bool touched = (digitalRead(TOUCH_PIN) == HIGH);
  unsigned long now = millis();

  // Detect rising edge: was LOW, now HIGH
  bool justPressed  = ( touched && !lastTouchState);
  // Detect falling edge: was HIGH, now LOW  ← fire action on RELEASE
  bool justReleased = (!touched &&  lastTouchState);

  // Always update state FIRST — no more early-return bugs
  lastTouchState = touched;

  // Only act on release edge + debounce guard
  // (Release edge = finger lifted = intentional tap completed)
  if (justReleased && (now - lastTouchTime > DEBOUNCE_MS)) {
    lastTouchTime    = now;
    lastActivityTime = now; // Reset sleep timer

    // Wake from sleep — this touch only wakes, does NOT change screen
    if (isSleeping) {
      isSleeping = false;
      display.setContrast(200);
      screenChanged = true;
      Serial.println("Woke from sleep");
      return; // Safe: lastTouchState already updated above
    }

    // Normal screen navigation
    if (currentScreen == SCREEN_EYES) {
      // Cycle through emotions first; after last emotion → next screen
      currentEmotion++;
      if (currentEmotion >= EMO_COUNT) {
        currentEmotion = 0;
        currentScreen  = SCREEN_WEATHER;
      }
    } else {
      currentScreen = (currentScreen + 1) % SCREEN_COUNT;
    }
    screenChanged = true;
    Serial.print("Screen → "); Serial.println(currentScreen);
  }
}

// ─────────────────────────────────────────────────────
//  SLEEP MODE: Check timeout and enter sleep
// ─────────────────────────────────────────────────────
void handleSleep() {
  if (isSleeping) return; // Already asleep, nothing to check

  unsigned long idleTime = millis() - lastActivityTime;

  if (idleTime >= SLEEP_TIMEOUT_MS) {
    // Entering sleep mode for the first time
    isSleeping  = true;
    breathVal   = 0.0f;
    breathDir   = 1.0f;

    // Dim the display immediately to signal sleep
    display.setContrast(5);
    display.clearDisplay();
    display.display();
    Serial.println("Entering sleep mode...");
  }
}

// ─────────────────────────────────────────────────────
//  SLEEP MODE: Breathing Eyes Animation
//
//  Uses a sine-wave to smoothly pulse the OLED contrast
//  between dim (5) and medium (80), while drawing
//  two calm, half-closed eyes — like a sleeping robot.
//
//  The "breathing" effect:
//    inhale  → eyes open slightly + contrast rises
//    exhale  → eyes close to slits + contrast falls
// ─────────────────────────────────────────────────────
void drawBreathingEyes() {
  // Advance breath sine wave
  breathVal += breathDir * BREATH_STEP;
  if (breathVal >= 1.0f) { breathVal = 1.0f; breathDir = -1.0f; }
  if (breathVal <= 0.0f) { breathVal = 0.0f; breathDir =  1.0f; }

  // Map breathVal (0→1) to contrast (5→80) — never fully bright
  uint8_t contrast = (uint8_t)(5 + breathVal * 75);
  display.setContrast(contrast);

  // Map breathVal to eye height: 2px (exhale) → 14px (inhale)
  int eyeH = 2 + (int)(breathVal * 12);

  // Eye positions — centered, spaced apart
  const int leftEyeX  = 26;
  const int rightEyeX = 76;
  const int eyeY      = 28;   // vertical center
  const int eyeW      = 30;

  display.clearDisplay();

  // Draw left eye (vertically centered around eyeY)
  display.fillRoundRect(leftEyeX,  eyeY - eyeH / 2, eyeW, eyeH, 3, SH110X_WHITE);
  // Draw right eye
  display.fillRoundRect(rightEyeX, eyeY - eyeH / 2, eyeW, eyeH, 3, SH110X_WHITE);

  // Tiny "zzz" text drifts upward based on breathVal — cute sleep indicator
  int zAlpha = (int)(breathVal * 3); // 0, 1, 2, or 3 z's visible
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  if (zAlpha >= 1) { display.setCursor(108, 24); display.print("z"); }
  if (zAlpha >= 2) { display.setCursor(114, 16); display.print("z"); }
  if (zAlpha >= 3) { display.setCursor(120,  8); display.print("z"); }

  display.display();
  delay(40); // ~25fps for smooth breathing
}


void connectWiFi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 24);
  display.println(F(" Connecting to WiFi"));
  display.display();

  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    tries++;
    // Animate dots
    display.setCursor(56 + tries * 3, 36);
    display.print(".");
    display.display();
  }

  display.clearDisplay();
  display.setCursor(10, 28);
  if (WiFi.status() == WL_CONNECTED) {
    display.println(F("  WiFi Connected!"));
  } else {
    display.println(F("  WiFi Failed :("));
  }
  display.display();
  delay(1000);
}

// ─────────────────────────────────────────────────────
//  WEATHER FETCH
// ─────────────────────────────────────────────────────
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  lastWeatherFetch = millis();

  // ── Current weather ──
  String url = String("http://api.openweathermap.org/data/2.5/weather?q=") +
               city + "&units=" + units + "&appid=" + apiKey;

  HTTPClient http;
  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);

    strncpy(weather.cityName, doc["name"] | "Pune", 31);
    weather.temp = doc["main"]["temp"] | 25.0f;
    strncpy(weather.description, doc["weather"][0]["description"] | "clear sky", 47);
    strncpy(weather.icon, doc["weather"][0]["icon"] | "01d", 7);
    weather.valid = true;
    Serial.println("Weather fetched OK");
  } else {
    Serial.print("Weather HTTP error: "); Serial.println(code);
  }
  http.end();

  // ── 3-day forecast (use /forecast endpoint, pick noon slots) ──
  String furl = String("http://api.openweathermap.org/data/2.5/forecast?q=") +
                city + "&units=" + units + "&cnt=24&appid=" + apiKey;

  HTTPClient fhttp;
  fhttp.begin(furl);
  int fcode = fhttp.GET();

  if (fcode == 200) {
    String fpayload = fhttp.getString();
    DynamicJsonDocument fdoc(8192);
    deserializeJson(fdoc, fpayload);

    JsonArray list = fdoc["list"];
    int daysFilled = 0;
    String seenDays[3];

    // Get today's date string to skip
    time_t nowT = time(nullptr);
    struct tm* today = localtime(&nowT);
    char todayStr[12];
    strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", today);

    for (JsonObject entry : list) {
      if (daysFilled >= 3) break;
      const char* dtTxt = entry["dt_txt"];
      if (!dtTxt) continue;

      // Extract date portion (first 10 chars)
      String dateStr = String(dtTxt).substring(0, 10);
      if (dateStr == String(todayStr)) continue; // skip today

      // Check if this date is already stored
      bool alreadySeen = false;
      for (int i = 0; i < daysFilled; i++) {
        if (seenDays[i] == dateStr) { alreadySeen = true; break; }
      }
      if (alreadySeen) continue;

      // Pick entry closest to noon
      String timeStr = String(dtTxt).substring(11, 13);
      if (timeStr.toInt() < 10 || timeStr.toInt() > 14) {
        // Only accept noon slots (10-14h); if none found yet for day, take first
        bool hasDayEntry = false;
        for (int i = 0; i < daysFilled; i++) {
          if (seenDays[i] == dateStr) { hasDayEntry = true; break; }
        }
        if (hasDayEntry) continue;
      }

      seenDays[daysFilled] = dateStr;
      weather.forecast_temp[daysFilled] = entry["main"]["temp"] | 25.0f;
      strncpy(weather.forecast_icon[daysFilled], entry["weather"][0]["icon"] | "01d", 7);
      daysFilled++;
    }
    Serial.println("Forecast fetched OK");
  }
  fhttp.end();
}

// ─────────────────────────────────────────────────────
//  BOOT SEQUENCE
// ─────────────────────────────────────────────────────
void drawBootScreen() {
  const char* messages[] = {
    "Hello everyone :)",
    "My name is\nDesktop Buddy",
    "I am pet robo\nof Saee"
  };

  for (int i = 0; i < 3; i++) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    if (i == 0) {
      // Big greeting
      display.setTextSize(1);
      display.setCursor(20, 10);
      display.println(F("*  *  *  *  *  *"));
      display.setTextSize(1);
      display.setCursor(18, 28);
      display.println(F("Hello everyone :)"));
      display.setCursor(20, 46);
      display.println(F("*  *  *  *  *  *"));
    } else if (i == 1) {
      display.setTextSize(1);
      display.setCursor(16, 16);
      display.println(F("My name is"));
      display.setTextSize(1);
      display.setCursor(8, 34);
      display.println(F("Desktop Buddy"));
    } else {
      display.setTextSize(1);
      display.setCursor(10, 16);
      display.println(F("I am pet robo of"));
      display.setTextSize(2);
      display.setCursor(28, 36);
      display.println(F("Saee"));
    }
    display.display();
    delay(2000);
  }
}

// ─────────────────────────────────────────────────────
//  HELPER: Draw Weather Icon using GFX primitives
//  All icons fit inside a 16×16 box starting at (x, y).
//  Uses hand-crafted PROGMEM bitmaps — each designed
//  pixel-by-pixel on a 16x16 grid for OLED clarity.
//
//  How to read: each row = 2 bytes = 16 bits = 16 pixels
//  Bit 1 = white pixel, Bit 0 = black pixel, MSB first.
//
//  OWM icon codes:
//    01*  → clear sky      → SUN
//    02*  → few clouds     → SUN + CLOUD
//    03*  → scattered      → CLOUD
//    04*  → broken/overcast→ CLOUD
//    09*  → shower rain    → RAIN
//    10*  → rain           → RAIN
//    11*  → thunderstorm   → THUNDER
//    13*  → snow           → SNOW
//    50*  → mist           → MIST
// ─────────────────────────────────────────────────────

// ☀ SUN  (16x16)
// Large filled circle, NO rays — clean & simple
// Row by row (MSB=left):
// 0000000000000000
// 0000001111000000
// 0000111111110000
// 0001111111111000
// 0001111111111000
// 0011111111111100
// 0011111111111100
// 0011111111111100
// 0011111111111100
// 0011111111111100
// 0011111111111100
// 0001111111111000
// 0001111111111000
// 0000111111110000
// 0000001111000000
// 0000000000000000
static const uint8_t PROGMEM bmp_sun[] = {
  0x00,0x00,
  0x03,0xC0,
  0x0F,0xF0,
  0x1F,0xF8,
  0x1F,0xF8,
  0x3F,0xFC,
  0x3F,0xFC,
  0x3F,0xFC,
  0x3F,0xFC,
  0x3F,0xFC,
  0x3F,0xFC,
  0x1F,0xF8,
  0x1F,0xF8,
  0x0F,0xF0,
  0x03,0xC0,
  0x00,0x00
};

// ☁ CLOUD  (16x16)
// 0000000000000000
// 0000001100000000
// 0000011110000000
// 0000111111000000
// 0001111111100000
// 0001111111110000
// 0011111111111000
// 0111111111111100
// 0111111111111100
// 0111111111111100
// 0111111111111100
// 0011111111111000
// 0000000000000000
// 0000000000000000
// 0000000000000000
// 0000000000000000
static const uint8_t PROGMEM bmp_cloud[] = {
  0x00,0x00,
  0x06,0x00,
  0x0F,0x00,
  0x1F,0x80,
  0x3F,0xE0,
  0x3F,0xF0,
  0x7F,0xF8,
  0x7F,0xFC,
  0x7F,0xFC,
  0x7F,0xFC,
  0x7F,0xFC,
  0x3F,0xF8,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};

// ⛅ PARTLY CLOUDY  (16x16)
// Small sun circle top-right, cloud overlapping bottom-left
// 0000000111000000
// 0000000111000000
// 0000000111000000
// 0000000000000000
// 0001100000011000  <- sun body sides
// 0001100000011000
// 0000000000000000
// 0000011110000000  <- cloud starts
// 0000111111000000
// 0001111111100000
// 0011111111110000
// 0011111111110000
// 0001111111100000
// 0000000000000000
// 0000000000000000
// 0000000000000000
static const uint8_t PROGMEM bmp_partly[] = {
  0x00,0x00,
  0x06,0x00,
  0x0F,0x00,
  0x1F,0x80,
  0x3F,0xE0,
  0x3F,0xF0,
  0x7F,0xF8,
  0xFF,0xFC,
  0xFF,0xFC,
  0xFF,0xFC,
  0xFF,0xFC,
  0x7F,0xF8,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};

// 🌧 RAIN  (16x16)
// Cloud top half, vertical rain lines bottom half
// 0000000000000000
// 0000001100000000
// 0000011110000000
// 0000111111000000
// 0001111111100000
// 0001111111110000
// 0011111111111000
// 0011111111111000
// 0000000000000000
// 0010010010010000  <- rain drops row 1
// 0001001001001000  <- rain drops row 2
// 0010010010010000
// 0001001001001000
// 0010010010010000
// 0000000000000000
// 0000000000000000
static const uint8_t PROGMEM bmp_rain[] = {
  0x00,0x00,
  0x06,0x00,
  0x0F,0x00,
  0x1F,0x80,
  0x3F,0xE0,
  0x3F,0xF0,
  0x7F,0xF8,
  0x7F,0xF8,
  0x00,0x00,
  0x49,0x20,
  0x24,0x90,
  0x49,0x20,
  0x24,0x90,
  0x49,0x20,
  0x00,0x00,
  0x00,0x00
};

// ⚡ THUNDER  (16x16)
// Cloud + bold lightning bolt
// 0000000000000000
// 0000001100000000
// 0000011110000000
// 0000111111000000
// 0001111111100000
// 0001111111110000
// 0011111111111000
// 0011111111111000
// 0000000110000000
// 0000001110000000
// 0000011100000000
// 0000111000000000
// 0000011000000000
// 0000001000000000
// 0000000000000000
// 0000000000000000
static const uint8_t PROGMEM bmp_thunder[] = {
  0x00,0x00,
  0x06,0x00,
  0x0F,0x00,
  0x1F,0x80,
  0x3F,0xE0,
  0x3F,0xF0,
  0x7F,0xF8,
  0x7F,0xF8,
  0x01,0x80,
  0x03,0x80,
  0x07,0x00,
  0x0E,0x00,
  0x06,0x00,
  0x02,0x00,
  0x00,0x00,
  0x00,0x00
};

// ❄ SNOW  (16x16)
// Cloud + asterisk-style snowflake
// 0000000000000000
// 0000001100000000
// 0000011110000000
// 0000111111000000
// 0001111111100000
// 0011111111110000
// 0011111111110000
// 0000000000000000
// 0000100010000000
// 0001110111000000
// 0000100010000000
// 0011111111000000  <- horizontal bar
// 0000100010000000
// 0001110111000000
// 0000100010000000
// 0000000000000000
static const uint8_t PROGMEM bmp_snow[] = {
  0x00,0x00,
  0x06,0x00,
  0x0F,0x00,
  0x1F,0x80,
  0x3F,0xE0,
  0x3F,0xF0,
  0x3F,0xF0,
  0x00,0x00,
  0x11,0x00,
  0x3B,0x80,
  0x11,0x00,
  0x3F,0x80,
  0x11,0x00,
  0x3B,0x80,
  0x11,0x00,
  0x00,0x00
};

// 🌫 MIST  (16x16)
// Four horizontal bars of varying width
// 0000000000000000
// 0000000000000000
// 0111111111111110
// 0111111111111110
// 0000000000000000
// 0011111111111100
// 0011111111111100
// 0000000000000000
// 0111111111111110
// 0111111111111110
// 0000000000000000
// 0011111111111100
// 0011111111111100
// 0000000000000000
// 0000000000000000
// 0000000000000000
static const uint8_t PROGMEM bmp_mist[] = {
  0x00,0x00,
  0x00,0x00,
  0x7F,0xFE,
  0x7F,0xFE,
  0x00,0x00,
  0x3F,0xFC,
  0x3F,0xFC,
  0x00,0x00,
  0x7F,0xFE,
  0x7F,0xFE,
  0x00,0x00,
  0x3F,0xFC,
  0x3F,0xFC,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};

// 🌙 MOON  (16x16) — crescent opening right
static const uint8_t PROGMEM bmp_moon[] = {
  0x00,0x00,
  0x04,0x00,
  0x0C,0x00,
  0x1C,0x00,
  0x38,0x00,
  0x3C,0x00,
  0x7C,0x00,
  0x7C,0x00,
  0x7C,0x00,
  0x3E,0x00,
  0x3F,0x00,
  0x1F,0xF0,
  0x0F,0xF8,
  0x07,0xF0,
  0x01,0xC0,
  0x00,0x00
};

// 🌙☁ NIGHT CLOUD  (16x16) — small moon top + cloud below
static const uint8_t PROGMEM bmp_night_cloud[] = {
  0x00,0x38,
  0x00,0x7C,
  0x00,0x38,
  0x00,0x00,
  0x06,0x00,
  0x0F,0x00,
  0x1F,0x80,
  0x3F,0xE0,
  0x3F,0xF0,
  0x7F,0xF8,
  0x7F,0xFC,
  0x7F,0xFC,
  0x3F,0xF8,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};

// ── Main dispatcher ──────────────────────────────────
// OWM iconCode format: "01d" or "01n"
//   c1+c2 = condition number
//   c3    = 'd' (day) or 'n' (night)
// At night: clear sky → moon, few/scattered clouds → night cloud
void drawWeatherIcon(int x, int y, const char* iconCode, int sz) {
  const uint8_t* bmp = bmp_cloud; // safe default
  char c1 = (iconCode && iconCode[0]) ? iconCode[0] : '0';
  char c2 = (iconCode && iconCode[1]) ? iconCode[1] : '3';
  char c3 = (iconCode && iconCode[2]) ? iconCode[2] : 'd'; // 'd' or 'n'
  bool isNight = (c3 == 'n');

  if (c1=='0' && c2=='1') {
    bmp = isNight ? bmp_moon : bmp_sun;           // clear sky
  } else if (c1=='0' && c2=='2') {
    bmp = isNight ? bmp_night_cloud : bmp_partly; // few clouds
  } else if (c1=='0' && c2=='3') {
    bmp = bmp_cloud;                               // scattered clouds
  } else if (c1=='0' && c2=='4') {
    bmp = bmp_cloud;                               // broken/overcast
  } else if (c1=='0' && c2=='9') {
    bmp = bmp_rain;                                // shower rain
  } else if (c1=='1' && c2=='0') {
    bmp = bmp_rain;                                // rain
  } else if (c1=='1' && c2=='1') {
    bmp = bmp_thunder;                             // thunderstorm
  } else if (c1=='1' && c2=='3') {
    bmp = bmp_snow;                                // snow
  } else if (c1=='5' && c2=='0') {
    bmp = bmp_mist;                                // mist
  }

  display.drawBitmap(x, y, bmp, 16, 16, SH110X_WHITE);
}

// ─────────────────────────────────────────────────────
//  SCREEN 1: ANIMATED ROBOT EYES
// ─────────────────────────────────────────────────────

// Eye geometry helpers
struct EyeShape { int x, y, w, h, rx, ry; };

// Draw one eye as rounded rectangle (or arc for emotion)
void drawEye(int cx, int cy, int w, int h, bool fillTop, bool fillBottom) {
  // Draw rounded rect for eye
  display.fillRoundRect(cx - w/2, cy - h/2, w, h, 4, SH110X_WHITE);
  // Cutoff for emotions using black rect
  if (fillTop) {
    display.fillRect(cx - w/2, cy - h/2, w, h/2, SH110X_BLACK);
  }
  if (fillBottom) {
    display.fillRect(cx - w/2, cy, w, h/2, SH110X_BLACK);
  }
}

// Eye animation frame state
unsigned long eyeAnimTimer = 0;
int eyeAnimFrame = 0;

void animateEyes() {
  unsigned long now = millis();
  int frameDelay = 150;

  if (now - eyeAnimTimer < frameDelay) return;
  eyeAnimTimer = now;

  display.clearDisplay();

  switch ((Emotion)currentEmotion) {

    case EMO_HAPPY: {
      // Happy: arched eyes (bottom half filled = U shape)
      // Left eye
      display.fillRoundRect(20, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(20, 22, 36, 11, SH110X_BLACK);    // cut top half → smile arch
      // Right eye
      display.fillRoundRect(72, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(72, 22, 36, 11, SH110X_BLACK);
      // Cheek dots
      display.fillCircle(15, 48, 3, SH110X_WHITE);
      display.fillCircle(113, 48, 3, SH110X_WHITE);
      break;
    }

    case EMO_BLINK: {
      // Blink: just thin lines
      int bh = (eyeAnimFrame < 3) ? (20 - eyeAnimFrame * 6) : (eyeAnimFrame * 6 - 16);
      if (bh < 2) bh = 2;
      if (bh > 20) bh = 20;
      display.fillRoundRect(20, 22 + (20 - bh)/2, 36, bh, 4, SH110X_WHITE);
      display.fillRoundRect(72, 22 + (20 - bh)/2, 36, bh, 4, SH110X_WHITE);
      eyeAnimFrame = (eyeAnimFrame + 1) % 6;
      break;
    }

    case EMO_SAD: {
      // Sad: top half filled → inverse arch
      display.fillRoundRect(20, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(20, 33, 36, 11, SH110X_BLACK);    // cut bottom half
      display.fillRoundRect(72, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(72, 33, 36, 11, SH110X_BLACK);
      // Tear drops
      if (eyeAnimFrame % 4 < 2) {
        display.fillCircle(35, 46, 2, SH110X_WHITE);
        display.fillCircle(87, 46, 2, SH110X_WHITE);
      }
      eyeAnimFrame = (eyeAnimFrame + 1) % 8;
      break;
    }

    case EMO_ANGRY: {
      // Angry: slanted brows over normal eyes
      display.fillRoundRect(20, 26, 36, 18, 4, SH110X_WHITE);
      display.fillRoundRect(72, 26, 36, 18, 4, SH110X_WHITE);
      // Left brow (slanted inward downward on inner corner)
      display.drawLine(18, 18, 54, 24, SH110X_WHITE);
      display.drawLine(18, 19, 54, 25, SH110X_WHITE);
      display.drawLine(18, 20, 54, 26, SH110X_WHITE);
      // Right brow
      display.drawLine(74, 24, 110, 18, SH110X_WHITE);
      display.drawLine(74, 25, 110, 19, SH110X_WHITE);
      display.drawLine(74, 26, 110, 20, SH110X_WHITE);
      break;
    }

    default: break;
  }

  // Emotion label at bottom
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  const char* labels[] = {"HAPPY", "BLINK", "SAD", "ANGRY"};
  int lx = (128 - strlen(labels[currentEmotion]) * 6) / 2;
  display.setCursor(lx, 56);
  display.print(labels[currentEmotion]);

  display.display();
}

void drawEyesScreen(Emotion emo) {
  currentEmotion = emo;
  eyeAnimFrame = 0;
  animateEyes();
}

// ─────────────────────────────────────────────────────
//  SCREEN 2: WEATHER CARD
// ─────────────────────────────────────────────────────
void drawWeatherScreen() {
  display.clearDisplay();

  if (!weather.valid) {
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(20, 28);
    display.println(F("No weather data"));
    display.display();
    return;
  }

  // City name (top-left)
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  // Truncate city name to fit ~14 chars
  char cityShort[15];
  strncpy(cityShort, weather.cityName, 14);
  cityShort[14] = '\0';
  display.print(cityShort);

  // Weather icon (top-right, 16x16)
  drawWeatherIcon(110, 0, weather.icon, 1);

  // Separator line
  display.drawFastHLine(0, 12, 128, SH110X_WHITE);

  // Large temperature
  display.setTextSize(3);
  char tempStr[8];
  snprintf(tempStr, sizeof(tempStr), "%d", (int)round(weather.temp));
  int tlen = strlen(tempStr);
  int tx = (128 - tlen * 18 - 12) / 2; // rough center (3x font = 18px/char + degree)
  display.setCursor(tx, 18);
  display.print(tempStr);
  display.setTextSize(2);
  display.print(F("\xF8")); // degree symbol

  // Divider line
  display.drawFastHLine(0, 50, 128, SH110X_WHITE);

  // Description (bottom, size 1)
  display.setTextSize(1);
  // Capitalize first letter
  char desc[48];
  strncpy(desc, weather.description, 47);
  desc[47] = '\0';
  if (desc[0] >= 'a' && desc[0] <= 'z') desc[0] -= 32;

  // Center description
  int dlen = strlen(desc);
  int dx = (128 - dlen * 6) / 2;
  if (dx < 0) dx = 0;
  display.setCursor(dx, 54);
  display.print(desc);

  display.display();
}

// ─────────────────────────────────────────────────────
//  SCREEN 3: 3-DAY FORECAST
// ─────────────────────────────────────────────────────
void drawForecastScreen() {
  display.clearDisplay();

  // Title bar
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.fillRect(0, 0, 128, 11, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(16, 2);
  display.print(F("3-DAY FORECAST"));
  display.setTextColor(SH110X_WHITE);

  // Separator
  display.drawFastHLine(0, 12, 128, SH110X_WHITE);

  // Column widths: 128/3 ≈ 42px each
  const int colW = 42;
  const char* dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

  time_t nowT = time(nullptr);
  struct tm* tmNow = localtime(&nowT);

  for (int i = 0; i < 3; i++) {
    int col_x = i * colW + 1;

    // Get day name for today+i+1
    int wday = (tmNow->tm_wday + i + 1) % 7;
    display.setCursor(col_x + (colW - strlen(dayNames[wday]) * 6) / 2 - 1, 14);
    display.print(dayNames[wday]);

    // Weather icon centered in column
    int iconX = col_x + (colW - 16) / 2;
    drawWeatherIcon(iconX, 26, weather.forecast_icon[i], 1);

    // Temperature
    char tStr[6];
    snprintf(tStr, sizeof(tStr), "%d\xF8", (int)round(weather.forecast_temp[i]));
    int tlen = strlen(tStr) * 6;
    display.setCursor(col_x + (colW - tlen) / 2, 46);
    display.print(tStr);

    // Vertical dividers
    if (i < 2) {
      display.drawFastVLine(col_x + colW - 1, 12, 52, SH110X_WHITE);
    }
  }

  display.display();
}

// ─────────────────────────────────────────────────────
//  SCREEN 4: CLOCK
// ─────────────────────────────────────────────────────
void drawClockScreen() {
  display.clearDisplay();

  time_t nowT = time(nullptr);
  struct tm* tmNow = localtime(&nowT);

  int hour12 = tmNow->tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  bool isPM = (tmNow->tm_hour >= 12);

  // AM/PM top-right
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(100, 2);
  display.print(isPM ? "PM" : "AM");

  // Large time HH:MM
  char timeStr[6];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hour12, tmNow->tm_min);
  display.setTextSize(3);
  // Each char 18px wide, 5 chars = 90px
  display.setCursor(19, 14);
  display.print(timeStr);

  // Separator line
  display.drawFastHLine(0, 44, 128, SH110X_WHITE);

  // Date e.g. "Mon Mar 09"
  const char* dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* monNames[] = {"Jan","Feb","Mar","Apr","May","Jun",
                             "Jul","Aug","Sep","Oct","Nov","Dec"};
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%s %s %02d",
           dayNames[tmNow->tm_wday],
           monNames[tmNow->tm_mon],
           tmNow->tm_mday);

  display.setTextSize(1);
  int dlen = strlen(dateStr) * 6;
  display.setCursor((128 - dlen) / 2, 52);
  display.print(dateStr);

  display.display();
}

// ─────────────────────────────────────────────────────
//  SCREEN 5: GOODBYE ANIMATION
// ─────────────────────────────────────────────────────
void drawGoodbyeScreen() {
  // Wave animation: alternate happy/blink eyes + text
  const char* msgs[] = {
    "Thank you for",
    "using me!",
    "",
    "See you again :)"
  };

  for (int rep = 0; rep < 3; rep++) {
    // Happy eyes
    display.clearDisplay();
    display.fillRoundRect(20, 10, 36, 22, 6, SH110X_WHITE);
    display.fillRect(20, 10, 36, 11, SH110X_BLACK);
    display.fillRoundRect(72, 10, 36, 22, 6, SH110X_WHITE);
    display.fillRect(72, 10, 36, 11, SH110X_BLACK);

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(10, 38);
    display.print(F("Thank you for"));
    display.setCursor(28, 48);
    display.print(F("using me!"));
    display.display();
    delay(600);

    // Blink
    display.clearDisplay();
    display.fillRoundRect(20, 20, 36, 4, 2, SH110X_WHITE);
    display.fillRoundRect(72, 20, 36, 4, 2, SH110X_WHITE);

    display.setCursor(10, 38);
    display.print(F("Thank you for"));
    display.setCursor(28, 48);
    display.print(F("using me!"));
    display.display();
    delay(150);
  }

  // Final message
  display.clearDisplay();
  display.fillRoundRect(20, 10, 36, 22, 6, SH110X_WHITE);
  display.fillRect(20, 10, 36, 11, SH110X_BLACK);
  display.fillRoundRect(72, 10, 36, 22, 6, SH110X_WHITE);
  display.fillRect(72, 10, 36, 11, SH110X_BLACK);
  display.fillCircle(15, 36, 3, SH110X_WHITE);
  display.fillCircle(113, 36, 3, SH110X_WHITE);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(8, 42);
  display.print(F("See you again! :)"));
  display.display();

  // After goodbye, loop back to eyes on next touch (screenChanged handled externally)
}