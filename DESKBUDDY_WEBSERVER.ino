/*
 * =====================================================
 *  DeskBuddy - ESP32 + SH1106 OLED Weather Robot
 *  Pet Robo of Saee
 * =====================================================
 *  Hardware:
 *    - ESP32-C3 Dev Board
 *    - 1.3" SH1106 OLED (I2C) at 0x3C
 *    - Touch Sensor on GPIO 4
 *    SDA → GPIO 6 | SCL → GPIO 7
 *
 *  Libraries needed (install via Library Manager):
 *    - Adafruit GFX Library
 *    - Adafruit SH110X
 *    - ArduinoJson (by Benoit Blanchon)
 *    - WiFi        (built-in ESP32)
 *    - HTTPClient  (built-in ESP32)
 *    - WebServer   (built-in ESP32)
 *
 *  NEW in this version:
 *    ✅ Web Dashboard at http://<ESP32-IP>/
 *    ✅ Live JSON data at http://<ESP32-IP>/data
 *    ✅ 4 Chart.js graphs (Temp, Humidity, Feels Like, Condition)
 *    ✅ Auto-refresh every 5 seconds
 *    ✅ Humidity + Feels Like fetched from OWM
 *    ✅ Night icons (moon) based on OWM day/night code
 *    ✅ Sleep mode with breathing eyes
 *    ✅ Touch debounce fixed (fires on release edge)
 * =====================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>        // ESP32 built-in web server
#include <ArduinoJson.h>
#include <time.h>

// ─── Display Config ───────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET    -1
#define I2C_ADDRESS   0x3C

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── Pin Config ───────────────────────────────────────
#define TOUCH_PIN 4

// ─── WiFi Credentials ─────────────────────────────────
const char* ssid     = "SAEE";        // ← Your WiFi name
const char* password = "12345678";    // ← Your WiFi password

// ─── OpenWeatherMap Config ────────────────────────────
const char* apiKey = "e6f441d09d758047e021eaa9a793c3e1";
const char* city   = "Pune";
const char* units  = "metric";

// ─── NTP Config ───────────────────────────────────────
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset = 19800;  // IST = UTC+5:30
const int   dstOffset = 0;

// ─── Web Server ───────────────────────────────────────
WebServer server(80);

// ─── Screen States ────────────────────────────────────
enum Screen {
  SCREEN_EYES = 0,
  SCREEN_WEATHER,
  SCREEN_FORECAST,
  SCREEN_CLOCK,
  SCREEN_GOODBYE,
  SCREEN_COUNT
};
int  currentScreen = SCREEN_EYES;
bool screenChanged = true;

// ─── Touch Debounce ───────────────────────────────────
unsigned long lastTouchTime  = 0;
const unsigned long DEBOUNCE_MS = 400;
bool lastTouchState = false;

// ─── Emotion States ───────────────────────────────────
enum Emotion { EMO_HAPPY = 0, EMO_BLINK, EMO_SAD, EMO_ANGRY, EMO_COUNT };
int currentEmotion = EMO_HAPPY;

// ─── Weather Data ─────────────────────────────────────
struct WeatherData {
  char  cityName[32];
  float temp;
  float feelsLike;          // feels like temperature
  float humidity;           // humidity %
  float windSpeed;          // wind speed m/s
  char  description[48];
  char  icon[8];            // OWM icon code e.g. "01d" / "01n"
  float forecast_temp[3];
  char  forecast_icon[3][8];
  bool  valid;
};
WeatherData weather;
unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_INTERVAL = 600000; // refresh every 10 min

// ─── History Arrays for Dashboard Graphs ──────────────
// 20 readings × 10 min = ~3.3 hours of data on graphs
#define HISTORY_SIZE 20
float hist_temp[HISTORY_SIZE];
float hist_humidity[HISTORY_SIZE];
float hist_feelsLike[HISTORY_SIZE];
float hist_condition[HISTORY_SIZE]; // 0=Clear 1=Clouds 2=Rain 3=Thunder 4=Snow 5=Mist
char  hist_time[HISTORY_SIZE][9];   // "HH:MM"
int   histIndex = 0;
int   histCount = 0;

// ─── Sleep Mode Config ────────────────────────────────
const unsigned long SLEEP_TIMEOUT_MS = 120000; // 2 minutes idle → sleep
unsigned long       lastActivityTime  = 0;
bool                isSleeping        = false;

// Breathing animation state
float breathVal  = 0.0f;
float breathDir  = 1.0f;
const float BREATH_STEP = 0.03f;

// ─── Forward Declarations ─────────────────────────────
void drawBootScreen();
void drawEyesScreen(Emotion emo);
void drawWeatherScreen();
void drawForecastScreen();
void drawClockScreen();
void drawGoodbyeScreen();
void fetchWeather();
void connectWiFi();
void startWebServer();
void handleRoot();
void handleData();
void recordHistory();
float descriptionToCode(const char* desc);
void drawWeatherIcon(int x, int y, const char* iconCode, int sz);
void animateEyes();
void handleTouch();
void handleSleep();
void drawBreathingEyes();

// ═════════════════════════════════════════════════════
//  WEATHER ICON BITMAPS  (16×16 pixels each, PROGMEM)
//  Each row = 2 bytes = 16 pixels. MSB = leftmost pixel.
// ═════════════════════════════════════════════════════

// ☀ SUN — filled circle
static const uint8_t PROGMEM bmp_sun[] = {
  0x00,0x00, 0x03,0xC0, 0x0F,0xF0, 0x1F,0xF8,
  0x1F,0xF8, 0x3F,0xFC, 0x3F,0xFC, 0x3F,0xFC,
  0x3F,0xFC, 0x3F,0xFC, 0x3F,0xFC, 0x1F,0xF8,
  0x1F,0xF8, 0x0F,0xF0, 0x03,0xC0, 0x00,0x00
};

// ☁ CLOUD — asymmetric bumpy cloud shape
static const uint8_t PROGMEM bmp_cloud[] = {
  0x00,0x00, 0x06,0x00, 0x0F,0x00, 0x1F,0x80,
  0x3F,0xE0, 0x3F,0xF0, 0x7F,0xF8, 0x7F,0xFC,
  0x7F,0xFC, 0x7F,0xFC, 0x7F,0xFC, 0x3F,0xF8,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

// ⛅ PARTLY CLOUDY — wider cloud (sun implied by extra width)
static const uint8_t PROGMEM bmp_partly[] = {
  0x00,0x00, 0x06,0x00, 0x0F,0x00, 0x1F,0x80,
  0x3F,0xE0, 0x3F,0xF0, 0x7F,0xF8, 0xFF,0xFC,
  0xFF,0xFC, 0xFF,0xFC, 0xFF,0xFC, 0x7F,0xF8,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

// 🌧 RAIN — cloud top + diagonal rain drops bottom
static const uint8_t PROGMEM bmp_rain[] = {
  0x00,0x00, 0x06,0x00, 0x0F,0x00, 0x1F,0x80,
  0x3F,0xE0, 0x3F,0xF0, 0x7F,0xF8, 0x7F,0xF8,
  0x00,0x00, 0x49,0x20, 0x24,0x90, 0x49,0x20,
  0x24,0x90, 0x49,0x20, 0x00,0x00, 0x00,0x00
};

// ⚡ THUNDER — cloud + lightning bolt
static const uint8_t PROGMEM bmp_thunder[] = {
  0x00,0x00, 0x06,0x00, 0x0F,0x00, 0x1F,0x80,
  0x3F,0xE0, 0x3F,0xF0, 0x7F,0xF8, 0x7F,0xF8,
  0x01,0x80, 0x03,0x80, 0x07,0x00, 0x0E,0x00,
  0x06,0x00, 0x02,0x00, 0x00,0x00, 0x00,0x00
};

// ❄ SNOW — cloud + snowflake asterisk
static const uint8_t PROGMEM bmp_snow[] = {
  0x00,0x00, 0x06,0x00, 0x0F,0x00, 0x1F,0x80,
  0x3F,0xE0, 0x3F,0xF0, 0x3F,0xF0, 0x00,0x00,
  0x11,0x00, 0x3B,0x80, 0x11,0x00, 0x3F,0x80,
  0x11,0x00, 0x3B,0x80, 0x11,0x00, 0x00,0x00
};

// 🌫 MIST — horizontal bar lines
static const uint8_t PROGMEM bmp_mist[] = {
  0x00,0x00, 0x00,0x00, 0x7F,0xFE, 0x7F,0xFE,
  0x00,0x00, 0x3F,0xFC, 0x3F,0xFC, 0x00,0x00,
  0x7F,0xFE, 0x7F,0xFE, 0x00,0x00, 0x3F,0xFC,
  0x3F,0xFC, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

// 🌙 MOON — crescent shape opening right
static const uint8_t PROGMEM bmp_moon[] = {
  0x00,0x00, 0x04,0x00, 0x0C,0x00, 0x1C,0x00,
  0x38,0x00, 0x3C,0x00, 0x7C,0x00, 0x7C,0x00,
  0x7C,0x00, 0x3E,0x00, 0x3F,0x00, 0x1F,0xF0,
  0x0F,0xF8, 0x07,0xF0, 0x01,0xC0, 0x00,0x00
};

// 🌙☁ NIGHT CLOUD — tiny moon top-right + cloud body
static const uint8_t PROGMEM bmp_night_cloud[] = {
  0x00,0x38, 0x00,0x7C, 0x00,0x38, 0x00,0x00,
  0x06,0x00, 0x0F,0x00, 0x1F,0x80, 0x3F,0xE0,
  0x3F,0xF0, 0x7F,0xF8, 0x7F,0xFC, 0x7F,0xFC,
  0x3F,0xF8, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

// ── Icon dispatcher ──────────────────────────────────
// OWM code: "01d"=sun, "01n"=moon, "02d"=partly, etc.
void drawWeatherIcon(int x, int y, const char* iconCode, int sz) {
  const uint8_t* bmp = bmp_cloud;
  char c1 = (iconCode && iconCode[0]) ? iconCode[0] : '0';
  char c2 = (iconCode && iconCode[1]) ? iconCode[1] : '3';
  char c3 = (iconCode && iconCode[2]) ? iconCode[2] : 'd';
  bool isNight = (c3 == 'n');

  if      (c1=='0' && c2=='1') bmp = isNight ? bmp_moon        : bmp_sun;
  else if (c1=='0' && c2=='2') bmp = isNight ? bmp_night_cloud : bmp_partly;
  else if (c1=='0' && c2=='3') bmp = bmp_cloud;
  else if (c1=='0' && c2=='4') bmp = bmp_cloud;
  else if (c1=='0' && c2=='9') bmp = bmp_rain;
  else if (c1=='1' && c2=='0') bmp = bmp_rain;
  else if (c1=='1' && c2=='1') bmp = bmp_thunder;
  else if (c1=='1' && c2=='3') bmp = bmp_snow;
  else if (c1=='5' && c2=='0') bmp = bmp_mist;

  display.drawBitmap(x, y, bmp, 16, 16, SH110X_WHITE);
}

// ═════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // ── Init OLED ──
  // ESP32-C3: SDA=GPIO6, SCL=GPIO7
  // Classic ESP32: SDA=GPIO21, SCL=GPIO22  ← change here if needed
  Wire.begin(6, 7);
  if (!display.begin(I2C_ADDRESS, true)) {
    Serial.println(F("SH1106 not found! Check wiring."));
    while (true) delay(1000);
  }
  display.setContrast(200);
  display.clearDisplay();
  display.display();

  // ── Touch pin: INPUT_PULLDOWN prevents floating-pin ghost touches ──
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // ── Boot animation ──
  drawBootScreen();
  lastActivityTime = millis();

  // ── WiFi ──
  connectWiFi();

  // ── NTP time sync ──
  configTime(gmtOffset, dstOffset, ntpServer);
  // Wait up to 5s for time to sync
  {
    time_t t = 0;
    int tries = 0;
    while (t < 100000 && tries < 10) { delay(500); t = time(nullptr); tries++; }
  }

  // ── First weather fetch ──
  fetchWeather();

  // ── Start web dashboard ──
  if (WiFi.status() == WL_CONNECTED) {
    startWebServer();
    Serial.print(F("Dashboard → http://"));
    Serial.println(WiFi.localIP());

    // Show IP on OLED for 3 seconds
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(14, 4);
    display.println(F("Web Dashboard:"));
    display.drawFastHLine(0, 14, 128, SH110X_WHITE);
    display.setCursor(4, 22);
    display.println(WiFi.localIP());
    display.setCursor(4, 40);
    display.println(F("Open in browser"));
    display.setCursor(4, 52);
    display.println(F("(same WiFi network)"));
    display.display();
    delay(3000);
  }
}

// ═════════════════════════════════════════════════════
//  MAIN LOOP
// ═════════════════════════════════════════════════════
void loop() {
  // Uncomment to debug raw touch pin in Serial Monitor:
  // Serial.println(digitalRead(TOUCH_PIN));

  handleTouch();
  handleSleep();

  // While sleeping: only run breathing animation
  if (isSleeping) {
    drawBreathingEyes();
    server.handleClient(); // keep dashboard alive even while sleeping
    delay(30);
    return;
  }

  // Handle browser requests
  server.handleClient();

  // Periodic weather refresh every 10 minutes
  if (millis() - lastWeatherFetch > WEATHER_INTERVAL) {
    fetchWeather();
  }

  // Redraw screen when navigation happens
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

  // Clock: tick every second
  if (currentScreen == SCREEN_CLOCK) {
    static unsigned long lastClockDraw = 0;
    if (millis() - lastClockDraw > 1000) {
      lastClockDraw = millis();
      drawClockScreen();
    }
  }

  // Eyes: animate continuously (has internal 150ms frame timer)
  if (currentScreen == SCREEN_EYES) {
    animateEyes();
  }

  delay(20);
}

// ═════════════════════════════════════════════════════
//  TOUCH HANDLING
//  Fires on RELEASE edge — much more reliable on
//  capacitive modules that hold HIGH for a while.
// ═════════════════════════════════════════════════════
void handleTouch() {
  bool touched      = (digitalRead(TOUCH_PIN) == HIGH);
  unsigned long now = millis();

  bool justReleased = (!touched && lastTouchState); // falling edge

  lastTouchState = touched; // ALWAYS update first — no early-return bugs

  if (justReleased && (now - lastTouchTime > DEBOUNCE_MS)) {
    lastTouchTime    = now;
    lastActivityTime = now;

    // Wake from sleep: only restores display, doesn't change screen
    if (isSleeping) {
      isSleeping = false;
      display.setContrast(200);
      screenChanged = true;
      Serial.println(F("Woke from sleep"));
      return;
    }

    // Navigate screens
    if (currentScreen == SCREEN_EYES) {
      currentEmotion++;
      if (currentEmotion >= EMO_COUNT) {
        currentEmotion = 0;
        currentScreen  = SCREEN_WEATHER;
      }
    } else {
      currentScreen = (currentScreen + 1) % SCREEN_COUNT;
    }
    screenChanged = true;
    Serial.print(F("Screen → ")); Serial.println(currentScreen);
  }
}

// ═════════════════════════════════════════════════════
//  SLEEP MODE
// ═════════════════════════════════════════════════════
void handleSleep() {
  if (isSleeping) return;
  if (millis() - lastActivityTime >= SLEEP_TIMEOUT_MS) {
    isSleeping = true;
    breathVal  = 0.0f;
    breathDir  = 1.0f;
    display.setContrast(5);
    display.clearDisplay();
    display.display();
    Serial.println(F("Entering sleep..."));
  }
}

void drawBreathingEyes() {
  breathVal += breathDir * BREATH_STEP;
  if (breathVal >= 1.0f) { breathVal = 1.0f; breathDir = -1.0f; }
  if (breathVal <= 0.0f) { breathVal = 0.0f; breathDir =  1.0f; }

  uint8_t contrast = (uint8_t)(5 + breathVal * 75);
  display.setContrast(contrast);

  int eyeH = 2 + (int)(breathVal * 12);

  display.clearDisplay();
  display.fillRoundRect(26, 28 - eyeH/2, 30, eyeH, 3, SH110X_WHITE);
  display.fillRoundRect(76, 28 - eyeH/2, 30, eyeH, 3, SH110X_WHITE);

  // Floating z's
  int z = (int)(breathVal * 3);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  if (z >= 1) { display.setCursor(108, 24); display.print("z"); }
  if (z >= 2) { display.setCursor(114, 16); display.print("z"); }
  if (z >= 3) { display.setCursor(120,  8); display.print("z"); }

  display.display();
  delay(40);
}

// ═════════════════════════════════════════════════════
//  WIFI CONNECTION
// ═════════════════════════════════════════════════════
void connectWiFi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(4, 24);
  display.println(F("Connecting to WiFi"));
  display.display();

  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    tries++;
    display.setCursor(56 + tries * 3, 38);
    display.print(".");
    display.display();
  }

  display.clearDisplay();
  display.setCursor(10, 28);
  if (WiFi.status() == WL_CONNECTED) {
    display.println(F(" WiFi Connected!"));
    Serial.print(F("IP: ")); Serial.println(WiFi.localIP());
  } else {
    display.println(F(" WiFi Failed :("));
    Serial.println(F("WiFi failed — dashboard disabled"));
  }
  display.display();
  delay(1000);
}

// ═════════════════════════════════════════════════════
//  WEATHER FETCH
//  Fetches temp, feelsLike, humidity, windSpeed,
//  description, icon — all in one API call.
// ═════════════════════════════════════════════════════
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  lastWeatherFetch = millis();

  // ── Current weather ──────────────────────────────
  String url = String("http://api.openweathermap.org/data/2.5/weather?q=")
             + city + "&units=" + units + "&appid=" + apiKey;

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);

    strncpy(weather.cityName,    doc["name"]                       | "Pune",      31);
    weather.temp      = doc["main"]["temp"]       | 25.0f;
    weather.feelsLike = doc["main"]["feels_like"] | 25.0f;
    weather.humidity  = doc["main"]["humidity"]   | 60.0f;
    weather.windSpeed = doc["wind"]["speed"]       | 0.0f;
    strncpy(weather.description, doc["weather"][0]["description"]  | "clear sky", 47);
    strncpy(weather.icon,        doc["weather"][0]["icon"]         | "01d",        7);
    weather.valid = true;

    recordHistory(); // store this reading for dashboard graphs
    Serial.println(F("Weather OK"));
  } else {
    Serial.print(F("Weather error: ")); Serial.println(code);
  }
  http.end();

  // ── 3-day forecast ────────────────────────────────
  String furl = String("http://api.openweathermap.org/data/2.5/forecast?q=")
              + city + "&units=" + units + "&cnt=24&appid=" + apiKey;

  HTTPClient fhttp;
  fhttp.begin(furl);
  int fcode = fhttp.GET();
  if (fcode == 200) {
    String fp = fhttp.getString();
    DynamicJsonDocument fdoc(8192);
    deserializeJson(fdoc, fp);

    JsonArray list    = fdoc["list"];
    int  daysFilled   = 0;
    String seenDays[3];

    time_t nowT = time(nullptr);
    struct tm* today = localtime(&nowT);
    char todayStr[12];
    strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", today);

    for (JsonObject entry : list) {
      if (daysFilled >= 3) break;
      const char* dtTxt = entry["dt_txt"];
      if (!dtTxt) continue;

      String dateStr = String(dtTxt).substring(0, 10);
      if (dateStr == String(todayStr)) continue;

      bool alreadySeen = false;
      for (int i = 0; i < daysFilled; i++)
        if (seenDays[i] == dateStr) { alreadySeen = true; break; }
      if (alreadySeen) continue;

      // Prefer noon slots (10-14h)
      int hr = String(dtTxt).substring(11, 13).toInt();
      bool hasDayEntry = false;
      for (int i = 0; i < daysFilled; i++)
        if (seenDays[i] == dateStr) { hasDayEntry = true; break; }
      if ((hr < 10 || hr > 14) && hasDayEntry) continue;

      seenDays[daysFilled] = dateStr;
      weather.forecast_temp[daysFilled] = entry["main"]["temp"] | 25.0f;
      strncpy(weather.forecast_icon[daysFilled],
              entry["weather"][0]["icon"] | "01d", 7);
      daysFilled++;
    }
    Serial.println(F("Forecast OK"));
  }
  fhttp.end();
}

// ═════════════════════════════════════════════════════
//  HISTORY RECORDING  (for dashboard graphs)
// ═════════════════════════════════════════════════════
float descriptionToCode(const char* desc) {
  String d = String(desc); d.toLowerCase();
  if (d.indexOf("clear")   >= 0) return 0;
  if (d.indexOf("cloud")   >= 0) return 1;
  if (d.indexOf("drizzle") >= 0) return 2;
  if (d.indexOf("rain")    >= 0) return 2;
  if (d.indexOf("thunder") >= 0) return 3;
  if (d.indexOf("snow")    >= 0) return 4;
  if (d.indexOf("mist")    >= 0) return 5;
  if (d.indexOf("fog")     >= 0) return 5;
  if (d.indexOf("haze")    >= 0) return 5;
  return 1;
}

void recordHistory() {
  time_t nowT = time(nullptr);
  struct tm* t = localtime(&nowT);
  snprintf(hist_time[histIndex], 9, "%02d:%02d", t->tm_hour, t->tm_min);

  hist_temp[histIndex]      = weather.temp;
  hist_humidity[histIndex]  = weather.humidity;
  hist_feelsLike[histIndex] = weather.feelsLike;
  hist_condition[histIndex] = descriptionToCode(weather.description);

  histIndex = (histIndex + 1) % HISTORY_SIZE;
  if (histCount < HISTORY_SIZE) histCount++;
}

// ═════════════════════════════════════════════════════
//  WEB DASHBOARD
// ═════════════════════════════════════════════════════

// /data — JSON polled by browser every 5 seconds
void handleData() {
  int start = (histCount < HISTORY_SIZE) ? 0 : histIndex;
  int count = histCount;

  String j = "{";
  j += "\"city\":\""        + String(weather.cityName)    + "\",";
  j += "\"temp\":"          + String(weather.temp, 1)     + ",";
  j += "\"humidity\":"      + String(weather.humidity, 1) + ",";
  j += "\"feelsLike\":"     + String(weather.feelsLike,1) + ",";
  j += "\"windSpeed\":"     + String(weather.windSpeed,1) + ",";
  j += "\"description\":\"" + String(weather.description) + "\",";

  time_t nowT = time(nullptr);
  struct tm* t = localtime(&nowT);
  char tb[9]; snprintf(tb, 9, "%02d:%02d", t->tm_hour, t->tm_min);
  j += "\"time\":\"" + String(tb) + "\",";

  // Labels
  j += "\"labels\":[";
  for (int i = 0; i < count; i++) {
    if (i) j += ",";
    j += "\"" + String(hist_time[(start+i)%HISTORY_SIZE]) + "\"";
  }
  j += "],";

  // Temperature history
  j += "\"tempH\":[";
  for (int i = 0; i < count; i++) {
    if (i) j += ",";
    j += String(hist_temp[(start+i)%HISTORY_SIZE], 1);
  }
  j += "],";

  // Humidity history
  j += "\"humH\":[";
  for (int i = 0; i < count; i++) {
    if (i) j += ",";
    j += String(hist_humidity[(start+i)%HISTORY_SIZE], 1);
  }
  j += "],";

  // Feels Like history
  j += "\"feelH\":[";
  for (int i = 0; i < count; i++) {
    if (i) j += ",";
    j += String(hist_feelsLike[(start+i)%HISTORY_SIZE], 1);
  }
  j += "],";

  // Condition history
  j += "\"condH\":[";
  for (int i = 0; i < count; i++) {
    if (i) j += ",";
    j += String(hist_condition[(start+i)%HISTORY_SIZE], 0);
  }
  j += "]";
  j += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

// / — Full dashboard HTML page
void handleRoot() {
  String html = F(R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desktop Buddy Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#0b0f1a;color:#e0e6f0;min-height:100vh}

/* ── Header ── */
header{
  background:linear-gradient(135deg,#111827,#1e3a5f);
  padding:18px 28px;
  display:flex;align-items:center;gap:14px;
  border-bottom:1px solid #1e3060;
  box-shadow:0 2px 16px #0005;
}
header .logo{font-size:2.2rem}
header h1{font-size:1.5rem;color:#60a5fa;letter-spacing:.5px}
header p{font-size:.8rem;color:#4a5568;margin-top:3px}
.live-badge{
  margin-left:auto;display:flex;align-items:center;gap:7px;
  background:#0f2;color:#000;padding:4px 12px;border-radius:20px;
  font-size:.75rem;font-weight:700;letter-spacing:1px;
}
.live-dot{
  width:8px;height:8px;border-radius:50%;background:#000;
  animation:blink 1.2s infinite;
}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}

/* ── Cards ── */
.cards{
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
  gap:14px;padding:22px 28px 0;
}
.card{
  background:#111827;border:1px solid #1e3060;border-radius:14px;
  padding:16px 14px;text-align:center;
  transition:transform .2s,box-shadow .2s;
}
.card:hover{transform:translateY(-3px);box-shadow:0 6px 24px #0008}
.card .icon{font-size:1.8rem;margin-bottom:6px}
.card .lbl{font-size:.7rem;color:#4a6080;text-transform:uppercase;letter-spacing:1px}
.card .val{font-size:1.65rem;font-weight:700;margin-top:4px}
.card.city .val{font-size:1.1rem;color:#60a5fa}
.card.temp .val{color:#fb923c}
.card.hum  .val{color:#38bdf8}
.card.feel .val{color:#c084fc}
.card.wind .val{color:#34d399}
.card.cond .val{font-size:.95rem;color:#fbbf24}
.card.time .val{font-size:1.3rem;color:#e2e8f0}

/* ── Charts ── */
.charts{
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(280px,1fr));
  gap:18px;padding:22px 28px 28px;
}
.chart-box{
  background:#111827;border:1px solid #1e3060;
  border-radius:14px;padding:18px 16px;
}
.chart-box h3{
  font-size:.78rem;color:#64748b;
  text-transform:uppercase;letter-spacing:1px;
  margin-bottom:12px;
}
canvas{max-height:200px}

footer{text-align:center;color:#1e3060;font-size:.72rem;padding:10px 0 18px}
</style>
</head>
<body>

<header>
  <div class="logo">🤖</div>
  <div>
    <h1>Desktop Buddy Dashboard</h1>
    <p>IoT Smart Desktop Assistant &mdash; Live Weather Monitor</p>
  </div>
  <div class="live-badge"><div class="live-dot"></div>LIVE</div>
</header>

<!-- Data cards -->
<div class="cards">
  <div class="card city"><div class="icon">📍</div><div class="lbl">City</div><div class="val" id="v-city">--</div></div>
  <div class="card temp"><div class="icon">🌡️</div><div class="lbl">Temperature</div><div class="val" id="v-temp">--</div></div>
  <div class="card hum"> <div class="icon">💧</div><div class="lbl">Humidity</div><div class="val" id="v-hum">--</div></div>
  <div class="card feel"><div class="icon">🤔</div><div class="lbl">Feels Like</div><div class="val" id="v-feel">--</div></div>
  <div class="card wind"><div class="icon">💨</div><div class="lbl">Wind Speed</div><div class="val" id="v-wind">--</div></div>
  <div class="card cond"><div class="icon">☁️</div><div class="lbl">Condition</div><div class="val" id="v-cond">--</div></div>
  <div class="card time"><div class="icon">🕐</div><div class="lbl">Last Update</div><div class="val" id="v-time">--</div></div>
</div>

<!-- Charts 2×2 -->
<div class="charts">
  <div class="chart-box"><h3>🌡 Temperature vs Time (°C)</h3><canvas id="cTemp"></canvas></div>
  <div class="chart-box"><h3>💧 Humidity vs Time (%)</h3><canvas id="cHum"></canvas></div>
  <div class="chart-box"><h3>🤔 Feels Like vs Time (°C)</h3><canvas id="cFeel"></canvas></div>
  <div class="chart-box"><h3>☁ Weather Condition vs Time</h3><canvas id="cCond"></canvas></div>
</div>

<footer>⟳ Auto-refreshes every 5 seconds &nbsp;|&nbsp; Desktop Buddy &mdash; Made by Saee</footer>

<script>
const COND_LABELS = ['Clear','Clouds','Rain','Thunder','Snow','Mist'];

// Shared grid / scale options
const gridColor = '#1a2540';
const textColor = '#4a6080';

function baseOpts(yLabel, tickCb) {
  return {
    responsive: true,
    animation: { duration: 500 },
    plugins: { legend: { labels: { color:'#94a3b8', font:{ size:11 } } } },
    scales: {
      x: { ticks:{ color:textColor, maxRotation:45, font:{size:10} }, grid:{ color:gridColor } },
      y: {
        ticks: { color:'#94a3b8', callback: tickCb||(v=>v) },
        grid:  { color:gridColor },
        title: { display:true, text:yLabel, color:textColor, font:{size:10} }
      }
    }
  };
}

function mkLine(id, label, color, yLabel, tickCb) {
  return new Chart(document.getElementById(id), {
    type: 'line',
    data: {
      labels: [],
      datasets:[{
        label, data:[],
        borderColor: color,
        backgroundColor: color+'28',
        borderWidth: 2.5,
        pointRadius: 4,
        pointHoverRadius: 6,
        fill: true,
        tension: 0.4
      }]
    },
    options: baseOpts(yLabel, tickCb)
  });
}

const charts = {
  temp : mkLine('cTemp', 'Temperature (°C)', '#fb923c', '°C'),
  hum  : mkLine('cHum',  'Humidity (%)',     '#38bdf8', '%'),
  feel : mkLine('cFeel', 'Feels Like (°C)',  '#c084fc', '°C'),
  cond : mkLine('cCond', 'Condition',        '#fbbf24', 'Type',
          v => COND_LABELS[Math.round(v)] || v)
};

function updateChart(chart, labels, data) {
  chart.data.labels = labels;
  chart.data.datasets[0].data = data;
  chart.update();
}

async function refresh() {
  try {
    const d = await fetch('/data').then(r => r.json());
    document.getElementById('v-city').textContent = d.city;
    document.getElementById('v-temp').textContent = d.temp + ' °C';
    document.getElementById('v-hum').textContent  = d.humidity + ' %';
    document.getElementById('v-feel').textContent = d.feelsLike + ' °C';
    document.getElementById('v-wind').textContent = d.windSpeed + ' m/s';
    document.getElementById('v-cond').textContent = d.description;
    document.getElementById('v-time').textContent = d.time;

    updateChart(charts.temp, d.labels, d.tempH);
    updateChart(charts.hum,  d.labels, d.humH);
    updateChart(charts.feel, d.labels, d.feelH);
    updateChart(charts.cond, d.labels, d.condH);
  } catch(e) { console.warn('Refresh error:', e); }
}

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>)rawhtml");

  server.send(200, "text/html", html);
}

// Register routes and start server
void startWebServer() {
  server.on("/",     handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println(F("Web server started on port 80"));
}

// ═════════════════════════════════════════════════════
//  BOOT SEQUENCE
// ═════════════════════════════════════════════════════
void drawBootScreen() {
  for (int i = 0; i < 3; i++) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    if (i == 0) {
      display.setTextSize(1);
      display.setCursor(20, 10); display.println(F("*  *  *  *  *  *"));
      display.setCursor(18, 28); display.println(F("Hello everyone :)"));
      display.setCursor(20, 46); display.println(F("*  *  *  *  *  *"));
    } else if (i == 1) {
      display.setTextSize(1);
      display.setCursor(16, 16); display.println(F("My name is"));
      display.setCursor(8,  34); display.println(F("Desktop Buddy"));
    } else {
      display.setTextSize(1);
      display.setCursor(10, 16); display.println(F("I am pet robo of"));
      display.setTextSize(2);
      display.setCursor(28, 36); display.println(F("Saee"));
    }
    display.display();
    delay(2000);
  }
}

// ═════════════════════════════════════════════════════
//  SCREEN 1: ANIMATED ROBOT EYES
// ═════════════════════════════════════════════════════
struct EyeShape { int x, y, w, h, rx, ry; };

void drawEye(int cx, int cy, int w, int h, bool fillTop, bool fillBottom) {
  display.fillRoundRect(cx-w/2, cy-h/2, w, h, 4, SH110X_WHITE);
  if (fillTop)    display.fillRect(cx-w/2, cy-h/2, w, h/2, SH110X_BLACK);
  if (fillBottom) display.fillRect(cx-w/2, cy,     w, h/2, SH110X_BLACK);
}

unsigned long eyeAnimTimer = 0;
int eyeAnimFrame = 0;

void animateEyes() {
  if (millis() - eyeAnimTimer < 150) return;
  eyeAnimTimer = millis();
  display.clearDisplay();

  switch ((Emotion)currentEmotion) {
    case EMO_HAPPY: {
      // Arched happy eyes with cheek blush
      display.fillRoundRect(20, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(20, 22, 36, 11, SH110X_BLACK);
      display.fillRoundRect(72, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(72, 22, 36, 11, SH110X_BLACK);
      display.fillCircle(15,  48, 3, SH110X_WHITE);
      display.fillCircle(113, 48, 3, SH110X_WHITE);
      break;
    }
    case EMO_BLINK: {
      // Eyes squish closed and open
      int bh = (eyeAnimFrame < 3) ? (20 - eyeAnimFrame*6) : (eyeAnimFrame*6 - 16);
      bh = max(2, min(20, bh));
      display.fillRoundRect(20, 22+(20-bh)/2, 36, bh, 4, SH110X_WHITE);
      display.fillRoundRect(72, 22+(20-bh)/2, 36, bh, 4, SH110X_WHITE);
      eyeAnimFrame = (eyeAnimFrame + 1) % 6;
      break;
    }
    case EMO_SAD: {
      // Droopy sad eyes with animated tears
      display.fillRoundRect(20, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(20, 33, 36, 11, SH110X_BLACK);
      display.fillRoundRect(72, 22, 36, 22, 6, SH110X_WHITE);
      display.fillRect(72, 33, 36, 11, SH110X_BLACK);
      if (eyeAnimFrame % 4 < 2) {
        display.fillCircle(35, 46, 2, SH110X_WHITE);
        display.fillCircle(87, 46, 2, SH110X_WHITE);
      }
      eyeAnimFrame = (eyeAnimFrame + 1) % 8;
      break;
    }
    case EMO_ANGRY: {
      // Eyes with slanted angry brows
      display.fillRoundRect(20, 26, 36, 18, 4, SH110X_WHITE);
      display.fillRoundRect(72, 26, 36, 18, 4, SH110X_WHITE);
      display.drawLine(18, 18, 54, 24, SH110X_WHITE);
      display.drawLine(18, 19, 54, 25, SH110X_WHITE);
      display.drawLine(18, 20, 54, 26, SH110X_WHITE);
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
  const char* labels[] = {"HAPPY","BLINK","SAD","ANGRY"};
  int lx = (128 - strlen(labels[currentEmotion])*6) / 2;
  display.setCursor(lx, 56);
  display.print(labels[currentEmotion]);
  display.display();
}

void drawEyesScreen(Emotion emo) {
  currentEmotion = emo;
  eyeAnimFrame   = 0;
  animateEyes();
}

// ═════════════════════════════════════════════════════
//  SCREEN 2: WEATHER CARD
// ═════════════════════════════════════════════════════
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

  // City name top-left
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  char cityShort[15]; strncpy(cityShort, weather.cityName, 14); cityShort[14]='\0';
  display.setCursor(0, 0); display.print(cityShort);

  // Weather icon top-right
  drawWeatherIcon(110, 0, weather.icon, 1);

  // Separator
  display.drawFastHLine(0, 12, 128, SH110X_WHITE);

  // Large temperature centre
  display.setTextSize(3);
  char tempStr[8]; snprintf(tempStr, 8, "%d", (int)round(weather.temp));
  int tx = (128 - strlen(tempStr)*18 - 12) / 2;
  display.setCursor(tx, 18); display.print(tempStr);
  display.setTextSize(2);    display.print(F("\xF8")); // degree symbol

  // Bottom separator
  display.drawFastHLine(0, 50, 128, SH110X_WHITE);

  // Description (left) + humidity (right) on bottom row
  display.setTextSize(1);
  char desc[48]; strncpy(desc, weather.description, 47); desc[47]='\0';
  if (desc[0]>='a' && desc[0]<='z') desc[0] -= 32;
  char descShort[13]; strncpy(descShort, desc, 12); descShort[12]='\0';
  display.setCursor(0, 54); display.print(descShort);

  char humStr[7]; snprintf(humStr, 7, "%d%%", (int)weather.humidity);
  display.setCursor(128 - strlen(humStr)*6, 54);
  display.print(humStr);

  display.display();
}

// ═════════════════════════════════════════════════════
//  SCREEN 3: 3-DAY FORECAST
// ═════════════════════════════════════════════════════
void drawForecastScreen() {
  display.clearDisplay();

  // Inverted title bar
  display.fillRect(0, 0, 128, 11, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(16, 2); display.print(F("3-DAY FORECAST"));
  display.setTextColor(SH110X_WHITE);
  display.drawFastHLine(0, 12, 128, SH110X_WHITE);

  const int colW = 42;
  const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  time_t nowT = time(nullptr);
  struct tm* tmNow = localtime(&nowT);

  for (int i = 0; i < 3; i++) {
    int cx   = i * colW + 1;
    int wday = (tmNow->tm_wday + i + 1) % 7;

    display.setCursor(cx + (colW - strlen(days[wday])*6)/2 - 1, 14);
    display.print(days[wday]);

    drawWeatherIcon(cx + (colW-16)/2, 26, weather.forecast_icon[i], 1);

    char ts[6]; snprintf(ts, 6, "%d\xF8", (int)round(weather.forecast_temp[i]));
    display.setCursor(cx + (colW - strlen(ts)*6)/2, 46);
    display.print(ts);

    if (i < 2) display.drawFastVLine(cx + colW - 1, 12, 52, SH110X_WHITE);
  }
  display.display();
}

// ═════════════════════════════════════════════════════
//  SCREEN 4: CLOCK
// ═════════════════════════════════════════════════════
void drawClockScreen() {
  display.clearDisplay();

  time_t nowT = time(nullptr);
  struct tm* t = localtime(&nowT);

  int  h12  = t->tm_hour % 12; if (h12==0) h12=12;
  bool isPM = (t->tm_hour >= 12);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(100, 2);
  display.print(isPM ? "PM" : "AM");

  char ts[6]; snprintf(ts, 6, "%02d:%02d", h12, t->tm_min);
  display.setTextSize(3);
  display.setCursor(19, 14);
  display.print(ts);

  display.drawFastHLine(0, 44, 128, SH110X_WHITE);

  const char* dn[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* mn[] = {"Jan","Feb","Mar","Apr","May","Jun",
                       "Jul","Aug","Sep","Oct","Nov","Dec"};
  char ds[16]; snprintf(ds, 16, "%s %s %02d", dn[t->tm_wday], mn[t->tm_mon], t->tm_mday);
  display.setTextSize(1);
  display.setCursor((128 - strlen(ds)*6)/2, 52);
  display.print(ds);
  display.display();
}

// ═════════════════════════════════════════════════════
//  SCREEN 5: GOODBYE ANIMATION
// ═════════════════════════════════════════════════════
void drawGoodbyeScreen() {
  for (int rep = 0; rep < 3; rep++) {
    // Happy wave eyes
    display.clearDisplay();
    display.fillRoundRect(20, 10, 36, 22, 6, SH110X_WHITE);
    display.fillRect(20, 10, 36, 11, SH110X_BLACK);
    display.fillRoundRect(72, 10, 36, 22, 6, SH110X_WHITE);
    display.fillRect(72, 10, 36, 11, SH110X_BLACK);
    display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(10, 38); display.print(F("Thank you for"));
    display.setCursor(28, 48); display.print(F("using me!"));
    display.display(); delay(600);

    // Quick blink
    display.clearDisplay();
    display.fillRoundRect(20, 20, 36, 4, 2, SH110X_WHITE);
    display.fillRoundRect(72, 20, 36, 4, 2, SH110X_WHITE);
    display.setCursor(10, 38); display.print(F("Thank you for"));
    display.setCursor(28, 48); display.print(F("using me!"));
    display.display(); delay(150);
  }

  // Final frame
  display.clearDisplay();
  display.fillRoundRect(20, 10, 36, 22, 6, SH110X_WHITE);
  display.fillRect(20, 10, 36, 11, SH110X_BLACK);
  display.fillRoundRect(72, 10, 36, 22, 6, SH110X_WHITE);
  display.fillRect(72, 10, 36, 11, SH110X_BLACK);
  display.fillCircle(15,  36, 3, SH110X_WHITE);
  display.fillCircle(113, 36, 3, SH110X_WHITE);
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(8, 42);
  display.print(F("See you again! :)"));
  display.display();
}