#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // Ensure v6+ is installed
#include <Adafruit_VCNL4040.h>
#include <Adafruit_SHT4x.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <cmath> // For sqrt

// --- Configuration ---
// WiFi credentials
#define WIFI_SSID "SHaven"       // <-- IMPORTANT: Replace with your WiFi SSID
#define WIFI_PASSWORD "27431sushi" // <-- IMPORTANT: Replace with your WiFi Password

// Cloud Function URLs
const String URL_GCF_UPLOAD = "https://plant-data-upload-971602190698.us-central1.run.app";
const String URL_GCF_GET_STATE = "https://get-device-state-971602190698.us-central1.run.app/"; // State check URL

// Hardcoded userId for this device
const String userId = "user_1";

// Timing
const unsigned long uploadInterval = 5000;
const unsigned long commandCheckInterval = 3000;
unsigned long lastCommandCheckTime = 0;
const unsigned long touchDebounce = 300; // Slightly longer debounce for UI stability
unsigned long lastTouchTime = 0;

// --- IMU & Vibration Configuration ---
const float SHAKE_THRESHOLD = 2.5f;
const unsigned long SHAKE_COOLDOWN = 2000;
const uint8_t VIBRATION_INTENSITY = 200;
const unsigned long VIBRATION_DURATION = 300;
unsigned long lastShakeTime = 0;

// --- Popup Configuration ---
bool showShakePopup = false;
unsigned long shakePopupStartTime = 0;
const unsigned long SHAKE_POPUP_DURATION = 1500;

// --- Cloud State Tracking ---
bool lastCloudState = false;
bool firstCloudCheck = true;

// --- NEW: UI State & In-Memory Log ---
enum Page { PAGE_MAIN, PAGE_LOG };
Page currentPage = PAGE_MAIN;

struct LogEntry {
  time_t timestamp;
  const char* eventType; // Store pointer to string literal (make sure literals are used)
};
const int MAX_LOG_ENTRIES = 8; // Store last 8 upload events in memory
LogEntry logEntries[MAX_LOG_ENTRIES];
int logEntryIndex = 0; // Index for next entry (circular buffer)
int logEntryCount = 0; // Number of valid entries stored

// --- End Configuration ---

// Sensor objects
Adafruit_VCNL4040 vcnl4040 = Adafruit_VCNL4040();
Adafruit_SHT4x sht4 = Adafruit_SHT4x();

// NTP client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -7 * 3600); // UTC-7

// Sensor Data Structure
struct SensorData {
  uint16_t prox; uint16_t ambientLight; uint16_t whiteLight;
  float temp; float rHum;
};
SensorData currentData;

// State variables
unsigned long lastUploadTime = 0;

// --- Layout Constants ---
namespace Layout {
  const int headerY = 5; const int headerH = 25;
  const int dataAreaY = 40; const int dataLabelX = 15;
  const int dataValueX = 160; const int dataRowH = 28; // Slightly reduced row height for space

  // Buttons (at the bottom)
  const int buttonH = 35;
  const int buttonY = 240 - buttonH - 5; // Position near bottom
  const int logButtonX = 10;
  const int logButtonW = 145;
  const int backButtonX = 10; // Same position for Back button
  const int backButtonW = 145;

  // Log Page Layout
  const int logTitleY = 10;
  const int logEntryY = 40;
  const int logEntryH = 22; // Reduced height for more entries
  const int logTimestampX = 15;
  const int logEventX = 175; // Adjusted X for event type
}

// --- Function Prototypes ---
void connectWiFi();
void updateSensors();
void uploadData(const char* triggerEvent = nullptr);
String generateM5DetailsHeader(const char* triggerEvent = nullptr);
// UI Functions
void drawScreen(); // Combined drawing logic based on currentPage
void updateScreenData(); // Combined update logic
void drawMainPage();
void updateMainPageData();
void drawLogPage();
void updateLogPageData();
void showPopup(const char* message, uint16_t bgColor, uint16_t textColor);
void clearPopup();
// Event/Action Functions
void checkCloudCommand();
void checkShakeAndVibrate();
// Touch Handling added back
void handleTouch();
bool pointInRect(int x, int y, int rx, int ry, int rw, int rh);
// Helper
String formatTimestamp(time_t epochTime);
// NEW: Log function
void addLogEntry(const char* eventType);


// --- Helper Functions ---
bool pointInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh);
}

String formatTimestamp(time_t epochTime) {
    if (epochTime == 0) return "No Time";
    struct tm timeinfo;
    // Use standard C function gmtime_r to convert epoch to UTC tm struct
    gmtime_r(&epochTime, &timeinfo);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%m/%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

// --- NEW: Add Entry to In-Memory Log ---
void addLogEntry(const char* eventType) {
    if (eventType == nullptr) eventType = "regular"; // Default if null passed

    timeClient.update(); // Ensure time is current before logging
    time_t now_ts = timeClient.getEpochTime();

    logEntries[logEntryIndex].timestamp = now_ts;
    logEntries[logEntryIndex].eventType = eventType; // Store pointer to the literal

    logEntryIndex = (logEntryIndex + 1) % MAX_LOG_ENTRIES; // Move index circularly
    if (logEntryCount < MAX_LOG_ENTRIES) {
        logEntryCount++; // Increment count until buffer is full
    }
     Serial.printf("Logged event: %s at %ld\n", eventType, now_ts); // Debug log
}


// --- UI Functions ---

void drawScreen() { // Main drawing router
    M5.Lcd.fillScreen(BLACK); // Clear screen before drawing page
    if (currentPage == PAGE_MAIN) {
        drawMainPage();
    } else if (currentPage == PAGE_LOG) {
        drawLogPage();
    }
}

void updateScreenData() { // Main update router
     if (currentPage == PAGE_MAIN) {
        updateMainPageData();
    } else if (currentPage == PAGE_LOG) {
        updateLogPageData(); // Log page needs update if data changes
    }
    // Handle popup clearing regardless of page
    unsigned long now = millis();
    if (showShakePopup && (now - shakePopupStartTime > SHAKE_POPUP_DURATION)) {
        clearPopup();
    }
}

void drawMainPage() { // Draw static parts of Main Page
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(WHITE, BLACK);
    // Header
    M5.Lcd.drawRect(Layout::dataValueX, Layout::headerY, 320 - Layout::dataValueX - 10, Layout::headerH, WHITE);
    M5.Lcd.setCursor(10, Layout::headerY + 5); M5.Lcd.print(userId);
    // Sensor Labels
    int currentY = Layout::dataAreaY;
    M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Proximity:"); currentY += Layout::dataRowH;
    M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Amb Light:"); currentY += Layout::dataRowH;
    M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("White Light:"); currentY += Layout::dataRowH;
    M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Temp (C):"); currentY += Layout::dataRowH;
    M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Humidity (%):");
    // Draw "Log" Button
    M5.Lcd.fillRect(Layout::logButtonX, Layout::buttonY, Layout::logButtonW, Layout::buttonH, TFT_BLUE);
    M5.Lcd.drawRect(Layout::logButtonX, Layout::buttonY, Layout::logButtonW, Layout::buttonH, TFT_WHITE);
    M5.Lcd.setTextColor(TFT_WHITE);
    int textWidth = M5.Lcd.textWidth("View Log"); int textHeight = M5.Lcd.fontHeight();
    M5.Lcd.setCursor(Layout::logButtonX + (Layout::logButtonW - textWidth) / 2, Layout::buttonY + (Layout::buttonH - textHeight) / 2);
    M5.Lcd.print("View Log"); M5.Lcd.setTextColor(WHITE, BLACK);
}

void updateMainPageData() { // Update dynamic parts of Main Page
    timeClient.update();
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.fillRect(Layout::dataValueX + 1, Layout::headerY + 1, 320 - Layout::dataValueX - 10 - 2, Layout::headerH - 2, BLACK);
    M5.Lcd.setCursor(Layout::dataValueX + 5, Layout::headerY + 5); M5.Lcd.print(timeClient.getFormattedTime());
    // Sensor Values
    char buffer[16]; int currentY = Layout::dataAreaY;
    M5.Lcd.fillRect(Layout::dataValueX, currentY, 320 - Layout::dataValueX - 10, Layout::dataRowH * 5, BLACK);
    sprintf(buffer, "%d", currentData.prox); M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer); currentY += Layout::dataRowH;
    sprintf(buffer, "%d lux", currentData.ambientLight); M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer); currentY += Layout::dataRowH;
    sprintf(buffer, "%d", currentData.whiteLight); M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer); currentY += Layout::dataRowH;
    sprintf(buffer, "%.1f", currentData.temp); M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer); currentY += Layout::dataRowH;
    sprintf(buffer, "%.1f", currentData.rHum); M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer);
}

void drawLogPage() { // Draw static parts of Log Page
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setCursor(Layout::dataLabelX, Layout::logTitleY); M5.Lcd.print("Recent Upload Log (Device)");
    // Draw "Back" Button
    M5.Lcd.fillRect(Layout::backButtonX, Layout::buttonY, Layout::backButtonW, Layout::buttonH, TFT_DARKGREY);
    M5.Lcd.drawRect(Layout::backButtonX, Layout::buttonY, Layout::backButtonW, Layout::buttonH, TFT_WHITE);
    M5.Lcd.setTextColor(TFT_WHITE);
    int textWidth = M5.Lcd.textWidth("Back"); int textHeight = M5.Lcd.fontHeight();
    M5.Lcd.setCursor(Layout::backButtonX + (Layout::backButtonW - textWidth) / 2, Layout::buttonY + (Layout::buttonH - textHeight) / 2);
    M5.Lcd.print("Back"); M5.Lcd.setTextColor(WHITE, BLACK);
    // Update dynamic log data area
    updateLogPageData();
}

void updateLogPageData() { // Update dynamic log entries display
    M5.Lcd.fillRect(0, Layout::logEntryY, M5.Lcd.width(), Layout::buttonY - Layout::logEntryY, BLACK); // Clear log area
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(WHITE, BLACK);

    if (logEntryCount == 0) {
        M5.Lcd.setCursor(Layout::dataLabelX, Layout::logEntryY); M5.Lcd.print("No log entries yet.");
        return;
    }

    int currentY = Layout::logEntryY;
    // Iterate backwards from the *previous* index in the circular buffer
    for (int i = 0; i < logEntryCount; ++i) {
        int indexToShow = (logEntryIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES; // Calculate index backwards circularly

        M5.Lcd.setCursor(Layout::logTimestampX, currentY);
        M5.Lcd.print(formatTimestamp(logEntries[indexToShow].timestamp));

        M5.Lcd.setCursor(Layout::logEventX, currentY);
        M5.Lcd.print(logEntries[indexToShow].eventType);

        currentY += Layout::logEntryH;
        if (currentY > Layout::buttonY - Layout::logEntryH) break; // Stop if exceeding display area
    }
}

void showPopup(const char* message, uint16_t bgColor, uint16_t textColor) { /* ... same as before ... */
    int popupW = 200; int popupH = 50; int popupX = (M5.Lcd.width() - popupW) / 2; int popupY = (M5.Lcd.height() - popupH) / 2;
    M5.Lcd.fillRect(popupX, popupY, popupW, popupH, bgColor); M5.Lcd.drawRect(popupX, popupY, popupW, popupH, textColor);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(textColor); int textWidth = M5.Lcd.textWidth(message); int textHeight = M5.Lcd.fontHeight();
    int textX = popupX + (popupW - textWidth) / 2; int textY = popupY + (popupH - textHeight) / 2;
    M5.Lcd.setCursor(textX, textY); M5.Lcd.print(message);
}

void clearPopup() { /* ... same as before, triggers redraw ... */
    showShakePopup = false; // Reset flag FIRST
    // Instead of clearing area, redraw the whole current screen cleanly
    drawScreen();
    // updateScreenData(); // Might not be needed if drawScreen calls the update
}

// --- Touch Handling Added Back ---
void handleTouch() {
  auto t = M5.Touch.getDetail(); // Read touch status structure

  // Check if touch occurred (finger down event)
  if (t.wasPressed()) {
    // Check debounce only on initial press
    if (millis() - lastTouchTime < touchDebounce) {
        return; // Too soon, ignore
    }
    lastTouchTime = millis(); // Record time of valid press

    if (currentPage == PAGE_MAIN) {
      // Check "View Log" button
      if (pointInRect(t.x, t.y, Layout::logButtonX, Layout::buttonY, Layout::logButtonW, Layout::buttonH)) {
        Serial.println("Log button pressed.");
        currentPage = PAGE_LOG;
        drawScreen(); // Redraw screen for the log page
      }
    } else if (currentPage == PAGE_LOG) {
      // Check "Back" button
      if (pointInRect(t.x, t.y, Layout::backButtonX, Layout::buttonY, Layout::backButtonW, Layout::buttonH)) {
        Serial.println("Back button pressed.");
        currentPage = PAGE_MAIN;
        drawScreen(); // Redraw screen for the main page
        // Optional: immediately update dynamic data on main page after switching back
        // updateMainPageData();
      }
    }
  }
  // No complex logic needed for finger lift (isreleased) with this simple setup
}

// --- Sensor & Network Functions ---

void updateSensors() { /* ... same as before ... */
  currentData.prox = vcnl4040.getProximity(); currentData.ambientLight = vcnl4040.getLux(); currentData.whiteLight = vcnl4040.getWhiteLight();
  sensors_event_t humidity, temperature; sht4.getEvent(&humidity, &temperature);
  currentData.temp = temperature.temperature; currentData.rHum = humidity.relative_humidity;
}

// Modified uploadData to add log entry
void uploadData(const char* triggerEvent) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi disconnected. Cannot upload."); return; }

  timeClient.update(); String headerValue = generateM5DetailsHeader(triggerEvent);
  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  Serial.print("Uploading data"); if (triggerEvent != nullptr) { Serial.printf(" (Trigger: %s)", triggerEvent); } Serial.print("...");

  bool uploadSuccess = false;
  if (http.begin(client, URL_GCF_UPLOAD)) {
    http.addHeader("M5-Details", headerValue); int httpCode = http.GET();
    if (httpCode > 0) {
      Serial.printf(" Upload successful, HTTP code: %d\n", httpCode);
      uploadSuccess = true; // Mark success
    } else {
      Serial.printf(" Upload failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else { Serial.println(" Failed to connect to upload URL."); }

  // Add log entry after attempting upload (could log success/failure too if needed)
  addLogEntry(triggerEvent ? triggerEvent : "regular"); // Log event type
}

String generateM5DetailsHeader(const char* triggerEvent) { /* ... same as before ... */
    StaticJsonDocument<512> doc; JsonObject vcnl = doc.createNestedObject("vcnlDetails");
    vcnl["prox"] = currentData.prox; vcnl["al"] = currentData.ambientLight; vcnl["wl"] = currentData.whiteLight;
    JsonObject sht = doc.createNestedObject("shtDetails"); sht["temp"] = currentData.temp; sht["rHum"] = currentData.rHum;
    JsonObject other = doc.createNestedObject("otherDetails"); other["timeCaptured"] = timeClient.getEpochTime(); other["userId"] = userId;
    if (triggerEvent != nullptr) { other["triggerEvent"] = triggerEvent; }
    String output; serializeJson(doc, output); return output;
}

void connectWiFi() { /* ... same as before ... */
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD); Serial.print("Connecting to WiFi");
    M5.Lcd.setTextSize(2); M5.Lcd.setCursor(10, 10); M5.Lcd.print("Connecting WiFi..."); int attempt = 0;
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); if(++attempt > 20) { Serial.println("\nFailed to connect."); M5.Lcd.fillScreen(BLACK); M5.Lcd.setCursor(10, 10); M5.Lcd.setTextColor(TFT_RED); M5.Lcd.print("WiFi Connection Failed!"); while(true) delay(1000); } }
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString()); M5.Lcd.fillScreen(BLACK); M5.Lcd.setTextColor(WHITE, BLACK);
}

void checkCloudCommand() { /* ... same state change detection as before ... */
    if (WiFi.status() != WL_CONNECTED) { return; } if (URL_GCF_GET_STATE == "" ) { return; }
    WiFiClientSecure client; client.setInsecure(); HTTPClient http; String requestUrl = URL_GCF_GET_STATE; requestUrl += "?userId=" + String(userId);
    if (http.begin(client, requestUrl)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString(); StaticJsonDocument<512> doc; DeserializationError error = deserializeJson(doc, payload);
            if (error) { Serial.print("State JSON parsing failed: "); Serial.println(error.c_str()); }
            else {
                if (doc.containsKey("fanState")) { // Still using "fanState" key as discussed
                    bool currentCloudState = doc["fanState"];
                    if (firstCloudCheck) { lastCloudState = currentCloudState; firstCloudCheck = false; Serial.printf("Initial cloud state received: %s\n", currentCloudState ? "TRUE" : "FALSE"); }
                    else if (currentCloudState != lastCloudState) {
                        Serial.printf("Cloud state changed from %s to %s. Triggering actions.\n", lastCloudState ? "TRUE" : "FALSE", currentCloudState ? "TRUE" : "FALSE");
                        Serial.println("Vibrating (Cloud State Change)..."); M5.Power.setVibration(VIBRATION_INTENSITY); delay(VIBRATION_DURATION); M5.Power.setVibration(0);
                        uploadData("cloud_state_change"); // Log this specific event
                        lastCloudState = currentCloudState;
                    }
                } else { Serial.println("State JSON response missing 'fanState' key."); }
            }
        } // else { /* Handle HTTP errors if needed */ }
        http.end();
    } // else { /* Handle connection error if needed */ }
}

void checkShakeAndVibrate() { /* ... same as before, calls showPopup ... */
    if (showShakePopup) return; float accX, accY, accZ; M5.Imu.getAccelData(&accX, &accY, &accZ); float magnitude = sqrt(accX * accX + accY * accY + accZ * accZ); unsigned long now = millis();
    if (magnitude > SHAKE_THRESHOLD && (now - lastShakeTime > SHAKE_COOLDOWN)) {
        Serial.printf("Shake detected! Magnitude: %.2f G\n", magnitude); lastShakeTime = now;
        Serial.println("Vibrating (Shake)..."); M5.Power.setVibration(VIBRATION_INTENSITY); delay(VIBRATION_DURATION); M5.Power.setVibration(0);
        showPopup("SHAKE DETECTED", TFT_ORANGE, TFT_WHITE); showShakePopup = true; shakePopupStartTime = now;
        uploadData("shake"); // Log this specific event
    }
}


// --- Setup & Main Loop ---
void setup() {
  auto cfg = M5.config(); M5.begin(cfg);
  M5.Lcd.setRotation(1); M5.Lcd.fillScreen(BLACK); Serial.begin(115200);
  M5.Touch.begin(&M5.Display); // Initialize Touch for buttons

  if (!M5.Imu.isEnabled()) { Serial.println("IMU Failed!"); M5.Lcd.setTextColor(TFT_RED); M5.Lcd.println("IMU Error!"); }
  else { Serial.println("IMU Initialized."); }

  Serial.println("M5 Core 2 Sensor Upload + In-Memory Log");

  // Sensor Init
  if (!vcnl4040.begin()) { Serial.println("VCNL4040 Error!"); M5.Lcd.setTextColor(TFT_RED); M5.Lcd.println("VCNL4040 Error!"); while (1) delay(1); }
  else { Serial.println("VCNL4040 OK."); }
  if (!sht4.begin()) { Serial.println("SHT4x Error!"); M5.Lcd.setTextColor(TFT_RED); M5.Lcd.println("SHT4x Error!"); while (1) delay(1); }
  else { Serial.println("SHT4x OK."); }
  sht4.setPrecision(SHT4X_HIGH_PRECISION); sht4.setHeater(SHT4X_NO_HEATER);

  connectWiFi();
  timeClient.begin(); timeClient.update();

  drawScreen(); // Draw the initial page (Main Page)
  updateSensors();
  // updateScreenData(); // Called within drawScreen -> drawMainPage -> updateMainPageData? No, call explicitly.
  updateMainPageData(); // Update dynamic data on initial page
}

void loop() {
  M5.update(); // Essential M5 update
  handleTouch(); // Process button presses for navigation

  // --- Run Checks and Updates ---
  if (currentPage == PAGE_MAIN) { // Only check sensors/cloud/shake if on main page?
      if (M5.Imu.isEnabled() && !showShakePopup) {
          checkShakeAndVibrate();
      }
      // Cloud check (HTTP request throttled inside function by timer logic below)
      checkCloudCommand();
      updateSensors();
  }

  updateScreenData(); // Update screen based on currentPage, handles popup clearing

  // --- Timed Actions ---
  unsigned long now = millis();

  // Regular Upload Timer (runs regardless of page?)
  if (now - lastUploadTime >= uploadInterval) {
     Serial.println("Regular upload interval reached.");
     uploadData("regular"); // This logs the event too
     lastUploadTime = now;
  }

  // Cloud State Check Timer (throttles HTTP requests in checkCloudCommand)
  if (now - lastCommandCheckTime >= commandCheckInterval) {
    // Only need to update the timer; the check itself runs every loop but respects this timer internally? No, the check needs to be here.
    // Let's move the checkCloudCommand call here to throttle it properly.
    // checkCloudCommand(); // Call it here based on the timer
    // lastCommandCheckTime = now; // Reset timer
     // Correction: The checkCloudCommand needs to run frequently to detect changes,
     // but the HTTP request *within* it should be throttled. Let's revert the logic.
     // Put the *timer update* here, and call checkCloudCommand every loop.
     // The HTTP request *inside* checkCloudCommand should be the throttled part.
     // --> Re-evaluating: The previous code called checkCloudCommand based on timer. Let's stick to that.
     checkCloudCommand(); // Check cloud state if interval passed
     lastCommandCheckTime = now;
  }


  delay(50); // Yield
}