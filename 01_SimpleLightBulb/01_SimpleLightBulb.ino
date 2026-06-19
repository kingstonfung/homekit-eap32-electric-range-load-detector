#include "HomeSpan.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>

#define LED_PIN       2
#define CONTACT_PIN   4

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_I2C_ADDR 0x3C

const unsigned long SCREEN_TIMEOUT_MS     = 5UL * 60UL * 1000UL; // 5 minutes 
const unsigned long CONTACT_OFF_DELAY_MS  = 90UL * 1000UL;        // 45 seconds
const unsigned long SCREEN_PROBE_INTERVAL = 2UL * 1000UL;         // 2 seconds
const char*         DISPLAY_TITLE         = "Elec. Range Load:";

const unsigned long INITIAL_MINUTES_FOR_FIRST_NOTIFICATAION = 45;
const unsigned long INTERVAL_MINUTES_FOR_NOTIFICATIONS      = 15;
const char*         NTFY_URL = "https://ntfy.sh/your-unique-topic-identifier-here";

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SpanCharacteristic* power;

// --- State ---
bool displayConnected = false;
bool screenOn         = false;
bool pendingOff       = false;

unsigned long lastActivityTime = 0;
unsigned long contactOpenTime  = 0;
unsigned long lastProbeTime    = 0;

bool          lastContactState = false;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 250UL;

// --- Stove-on notification timer state ---
bool          ledIsOn           = false;
unsigned long ledOnStartTime    = 0;
unsigned int  notificationsSent = 0;

// ---------- helpers ----------

inline bool elapsed(unsigned long timestamp, unsigned long interval) {
  return (millis() - timestamp) >= interval;
}

bool probeI2C(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ---------- stove-on notification timer ----------

void sendStoveNotification(unsigned long minutesOn) {
  if (!WiFi.isConnected()) {
    Serial.println("WiFi not connected — skipping notification POST");
    return;
  }

  HTTPClient http;
  http.begin(NTFY_URL);

  String body = "Stove is on for " + String(minutesOn) + " minutes";
  int httpCode = http.POST(body);

  Serial.printf("ntfy POST (%lu min) -> HTTP %d\n", minutesOn, httpCode);

  http.end();
}

// Call whenever the LED's actual on/off state changes (turns the
// notification timer on or off and resets it).
void setLED(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);

  if (on && !ledIsOn) {
    ledIsOn           = true;
    ledOnStartTime    = millis();
    notificationsSent = 0;
  } else if (!on && ledIsOn) {
    ledIsOn           = false;
    ledOnStartTime    = 0;
    notificationsSent = 0;
  }
}

// Call every loop() iteration; fires the first notification after
// INITIAL_MINUTES_FOR_FIRST_NOTIFICATAION minutes, then again every
// INTERVAL_MINUTES_FOR_NOTIFICATIONS minutes after that, for as long
// as the LED stays on.
void checkStoveNotifications() {
  if (!ledIsOn) return;

  unsigned long elapsedMs = millis() - ledOnStartTime;

  unsigned long nextThresholdMinutes =
      INITIAL_MINUTES_FOR_FIRST_NOTIFICATAION +
      (notificationsSent * INTERVAL_MINUTES_FOR_NOTIFICATIONS);

  unsigned long nextThresholdMs = nextThresholdMinutes * 60UL * 1000UL;

  if (elapsedMs >= nextThresholdMs) {
    notificationsSent++;
    sendStoveNotification(nextThresholdMinutes);
  }
}

void setScreenPower(bool on) {
  if (on == screenOn) return;
  display.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
  screenOn = on;
}

void updateDisplay(bool isOn, const char* source) {
  if (!displayConnected) return;

  if (!screenOn) {
    setScreenPower(true);
  }
  lastActivityTime = millis();

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(DISPLAY_TITLE);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  bool networkUp = WiFi.isConnected();

  display.setTextSize(1);
  display.setCursor(0, 16);
  display.print(networkUp ? "WiFi: Connected"
                          : "WiFi: No network");

  display.setTextSize(3);
  display.setCursor(28, 28);
  display.println(isOn ? "ON" : "OFF");

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.printf("via %s", source);

  display.display();
}

bool initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    return false;
  }
  display.clearDisplay();
  display.display();
  return true;
}

void checkDisplayConnection() {
  if (!elapsed(lastProbeTime, SCREEN_PROBE_INTERVAL)) return;
  lastProbeTime = millis();

  bool detected = probeI2C(OLED_I2C_ADDR);

  if (!displayConnected && detected) {
    Serial.println("Display reconnected — reinitialising");
    if (initDisplay()) {
      displayConnected = true;
      screenOn         = false;
      setScreenPower(true);
      lastActivityTime = millis();
      updateDisplay((bool)power->getVal(), "rewired");
    }
  } else if (displayConnected && !detected) {
    Serial.println("Display disconnected");
    displayConnected = false;
    screenOn         = false;
  }
}

// ---------- HomeKit accessory ----------

struct MyLightBulb : Service::LightBulb {

  MyLightBulb() : Service::LightBulb() {
    power = new Characteristic::On();
  }

  boolean update() override {
    bool isOn = power->getNewVal();
    Serial.printf("HomeKit set light %s\n", isOn ? "ON" : "OFF");
    setLED(isOn);
    updateDisplay(isOn, "HomeKit");
    if (isOn) pendingOff = false;
    return true;
  }
};

struct RestartSwitch : Service::Switch {

  SpanCharacteristic *power;

  RestartSwitch() : Service::Switch() {
    power = new Characteristic::On(false);
  }

  boolean update() override {
    if (power->getNewVal()) {
      Serial.println("Hardware-level restart requested from HomeKit");

      // 1. Instantly wipe the display to pure black
      if (displayConnected) {
        display.clearDisplay();
        display.display();
        setScreenPower(false); // Put the screen into low-power sleep mode
      }

      // 2. Instantly kill the physical LED pin
      setLED(false);

      // 3. Clear HomeSpan state memory so it boots up as OFF
      if (::power != nullptr) {
        ::power->setVal(false);
      }

      // 4. Reset this switch back to OFF in HomeKit before the crash
      power->setVal(false); 

      Serial.println("Triggering hardware Watchdog Reset now...");
      delay(500); // Small pause to allow the display/serial commands to finish

      // 5. Force a hardware panic reset by initializing a 1ms watchdog timer
      // and letting it expire without "feeding" it.
      esp_task_wdt_config_t twdt_config = {
          .timeout_ms = 1,
          .idle_core_mask = 0,
          .trigger_panic = true
      };
      esp_task_wdt_reconfigure(&twdt_config);
      esp_task_wdt_add(NULL);
      
      while(true); // Infinite trap loop to force the watchdog to instantly reset the CPU hardware
    }

    return true;
  }
};

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,     OUTPUT);
  pinMode(CONTACT_PIN, INPUT_PULLUP);

  Wire.begin();

  if (initDisplay()) {
    displayConnected = true;
    screenOn         = true;
    lastActivityTime = millis();
    updateDisplay(false, "startup");
  } else {
    Serial.println("SSD1306 not found — continuing without display");
    displayConnected = false;
    screenOn         = false;
  }

  homeSpan.enableOTA();
  homeSpan.begin(Category::Lighting, DISPLAY_TITLE, "StoveSensor");

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();    
    // FIX: Set default to false (OFF) and set the second argument to false 
    // to prevent HomeSpan from loading any previously cached "ON" state.
    power = new Characteristic::On(false, false); 
    new MyLightBulb();

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
    new RestartSwitch();
}

void loop() {
  homeSpan.poll();

  // --- Display reconnection probe ---
  checkDisplayConnection();

  // --- Stove-on notification timer ---
  checkStoveNotifications();

  // --- Detect WiFi state changes ---
  static bool lastWiFiState = false;

  bool wifiNow = WiFi.isConnected();

  if (wifiNow != lastWiFiState) {
    lastWiFiState = wifiNow;
    updateDisplay((bool)power->getVal(), "wifi");
  }

  // --- Screen timeout ---
  if (displayConnected &&
      screenOn &&
      elapsed(lastActivityTime, SCREEN_TIMEOUT_MS)) {
    setScreenPower(false);
  }

  // --- Delayed contact-off ---
  if (pendingOff &&
      elapsed(contactOpenTime, CONTACT_OFF_DELAY_MS)) {

    pendingOff = false;

    Serial.println("Contact delay elapsed — turning OFF");

    power->setVal(false);
    setLED(false);

    updateDisplay(false, "contact");
  }

  // --- Contact sensor ---
  bool contactClosed = (digitalRead(CONTACT_PIN) == LOW);

  if (contactClosed != lastContactState &&
      elapsed(lastDebounceTime, DEBOUNCE_DELAY)) {

    lastContactState = contactClosed;
    lastDebounceTime = millis();

    if (contactClosed) {

      pendingOff = false;

      Serial.println("Contact CLOSED — turning ON");

      power->setVal(true);
      setLED(true);

      updateDisplay(true, "contact");

    } else {

      pendingOff      = true;
      contactOpenTime = millis();

      Serial.printf(
        "Contact OPEN — turning OFF in %lu seconds\n",
        CONTACT_OFF_DELAY_MS / 1000UL
      );
    }
  }
}
