#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_VCNL4040.h>
#include <Adafruit_SHT4x.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

// --- Configuration ---
// WiFi credentials
#define WIFI_SSID "SHaven"       // <-- IMPORTANT: Replace with your WiFi SSID
#define WIFI_PASSWORD "27431sushi" // <-- IMPORTANT: Replace with your WiFi Password

// Cloud Run upload endpoint URL
const String URL_GCF_UPLOAD = "https://plant-data-upload-971602190698.us-central1.run.app";
const String URL_GCF_GET_STATE = "https://get-device-state-971602190698.us-central1.run.app/";

// Hardcoded userId for this device (change if needed)
const String userId = "user_1";

// Timing
const unsigned long uploadInterval = 5000; // Upload data every 5 seconds (5000 ms)
const unsigned long touchDebounce = 200;   // Debounce interval for touch input (ms)
// Timing for checking commands
const unsigned long commandCheckInterval = 3000; // Check for new commands every 3 seconds (3000 ms)
unsigned long lastCommandCheckTime = 0;
// --- End Configuration ---

// Sensor objects
Adafruit_VCNL4040 vcnl4040 = Adafruit_VCNL4040();
Adafruit_SHT4x sht4 = Adafruit_SHT4x();

// NTP client for current time (UTC-7 for PDT/MST without DST, adjust offset if needed)
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -7 * 3600); // -7 * 3600 seconds offset for UTC-7

// Structure to hold sensor data (Simplified)
struct SensorData {
  uint16_t prox;
  uint16_t ambientLight;
  uint16_t whiteLight;
  float temp;
  float rHum;
};

SensorData currentData;

// State variables
unsigned long lastUploadTime = 0;
unsigned long lastTouchTime = 0;
bool fanState = false; // false = OFF, true = ON
static bool fanBtnHeld = false;

// --- Layout Constants ---
namespace Layout {
  const int headerY = 5;
  const int headerH = 25;
  const int dataAreaY = 40;
  const int dataLabelX = 15;
  const int dataValueX = 160; // X position for sensor values
  const int dataRowH = 30;    // Height/spacing for each sensor row
  const int fanButtonY = 190;
  const int fanButtonH = 40;
  const int fanButtonX = 10;
  const int fanButtonW = 300;
}

// --- Function Prototypes ---
void connectWiFi();
void updateSensors();
void uploadData();
String generateM5DetailsHeader();
void drawScreen();
void updateScreenData();
void handleTouch();
bool pointInRect(int x, int y, int rx, int ry, int rw, int rh);
void drawFanButton(); // Specific function to draw/update the fan button
void checkCloudCommand(); // for fan state

// --- Helper Functions ---

// Returns true if (x, y) lies inside the given rectangle.
bool pointInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh);
}

// Draws the fan button based on its current state
void drawFanButton() {
    M5.Lcd.setTextSize(2);
    int x = Layout::fanButtonX;
    int y = Layout::fanButtonY;
    int w = Layout::fanButtonW;
    int h = Layout::fanButtonH;
    uint16_t bgColor = fanState ? TFT_GREEN : TFT_RED; // Green if ON, Red if OFF
    uint16_t textColor = TFT_WHITE;
    String btnText = fanState ? "Fan: ON" : "Fan: OFF";

    M5.Lcd.fillRect(x, y, w, h, bgColor);
    M5.Lcd.drawRect(x, y, w, h, textColor); // Outline

    int textWidth = M5.Lcd.textWidth(btnText);
    int textHeight = M5.Lcd.fontHeight(); // Use fontHeight for vertical centering
    int textX = x + (w - textWidth) / 2;
    int textY = y + (h - textHeight) / 2;

    M5.Lcd.setCursor(textX, textY);
    M5.Lcd.setTextColor(textColor);
    M5.Lcd.print(btnText);
}

// --- UI Functions ---

// Draws the static parts of the screen
void drawScreen() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK); // White text, Black background

  // Header Placeholder (Time will be updated dynamically)
  M5.Lcd.drawRect(Layout::dataValueX, Layout::headerY, 320 - Layout::dataValueX - 10, Layout::headerH, WHITE);
  M5.Lcd.setCursor(10, Layout::headerY + 5);
  M5.Lcd.print(userId);


  // Sensor Labels
  int currentY = Layout::dataAreaY;
  M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Proximity:");
  currentY += Layout::dataRowH;
  M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Amb Light:");
  currentY += Layout::dataRowH;
  M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("White Light:");
  currentY += Layout::dataRowH;
  M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Temp (C):");
  currentY += Layout::dataRowH;
  M5.Lcd.setCursor(Layout::dataLabelX, currentY); M5.Lcd.print("Humidity (%):");

  // Draw initial Fan Button state
  drawFanButton();
}

// Updates dynamic data on the screen (sensor values, time)
void updateScreenData() {
  timeClient.update();

  // Update Time
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.fillRect(Layout::dataValueX + 1, Layout::headerY + 1, 320 - Layout::dataValueX - 10 - 2, Layout::headerH - 2, BLACK); // Clear old time
  M5.Lcd.setCursor(Layout::dataValueX + 5, Layout::headerY + 5);
  M5.Lcd.print(timeClient.getFormattedTime());

  // Update Sensor Values
  char buffer[16];
  int currentY = Layout::dataAreaY;

  // Clear old values before drawing new ones
  M5.Lcd.fillRect(Layout::dataValueX, currentY, 320 - Layout::dataValueX - 10, Layout::dataRowH * 5, BLACK);

  // Proximity
  sprintf(buffer, "%d", currentData.prox);
  M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer);
  currentY += Layout::dataRowH;

  // Ambient Light
  sprintf(buffer, "%d lux", currentData.ambientLight); // Added units
  M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer);
  currentY += Layout::dataRowH;

  // White Light
  sprintf(buffer, "%d", currentData.whiteLight);
  M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer);
  currentY += Layout::dataRowH;

  // Temperature
  sprintf(buffer, "%.1f", currentData.temp);
  M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer);
  currentY += Layout::dataRowH;

  // Humidity
  sprintf(buffer, "%.1f", currentData.rHum);
  M5.Lcd.setCursor(Layout::dataValueX, currentY); M5.Lcd.print(buffer);
}

// Handles touch input
void handleTouch() {
  if (M5.Touch.getCount() > 0) {
    auto pos = M5.Touch.getDetail(0);
    if (pointInRect(pos.x, pos.y,
                    Layout::fanButtonX, Layout::fanButtonY,
                    Layout::fanButtonW, Layout::fanButtonH)) {
      // only on first down, not while still held
      if (!fanBtnHeld && millis() - lastTouchTime >= touchDebounce) {
        fanBtnHeld     = true;
        lastTouchTime  = millis();
        fanState       = !fanState;
        drawFanButton();
        Serial.printf("Fan toggled: %s\n", fanState ? "ON":"OFF");
      }
    }
  } else {
    // finger lifted, allow next press
    fanBtnHeld = false;
  }
}

// --- Sensor & Network Functions ---

// Reads data from VCNL and SHT sensors
void updateSensors() {
  currentData.prox = vcnl4040.getProximity();
  currentData.ambientLight = vcnl4040.getLux();     // Using getLux() for ambient light
  currentData.whiteLight = vcnl4040.getWhiteLight();

  sensors_event_t humidity, temperature;
  sht4.getEvent(&humidity, &temperature); // Gets both readings
  currentData.temp = temperature.temperature;
  currentData.rHum = humidity.relative_humidity;

   // Optional: Print to Serial for debugging
   // Serial.printf("Prox: %d, Lux: %d, White: %d, Temp: %.1f C, Hum: %.1f %%\n",
   //              currentData.prox, currentData.ambientLight, currentData.whiteLight,
   //              currentData.temp, currentData.rHum);
}

// Uploads sensor data to the cloud function
void uploadData() {
  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Cannot upload.");
      // Optional: Try to reconnect?
      // connectWiFi();
      return;
  }

  timeClient.update(); // Ensure time is current before generating header
  // updateSensors(); // Sensors are updated in loop, maybe not needed again here unless upload interval is very long

  String headerValue = generateM5DetailsHeader();

  WiFiClientSecure client;
  client.setInsecure(); // Disable certificate verification for simplicity (not recommended for production)
  HTTPClient http;

  Serial.print("Uploading data...");
  if (http.begin(client, URL_GCF_UPLOAD)) {
    http.addHeader("M5-Details", headerValue);
    int httpCode = http.GET(); // Using GET as per original code

    if (httpCode > 0) {
      Serial.printf(" Upload successful, HTTP code: %d\n", httpCode);
      // String payload = http.getString(); // Optional: read response
      // Serial.println("Response: " + payload);
    } else {
      Serial.printf(" Upload failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println(" Failed to connect to upload URL.");
  }
}

// Generates the JSON string for the M5-Details header (Simplified)
String generateM5DetailsHeader() {
  StaticJsonDocument<512> doc;

  JsonObject vcnl = doc.createNestedObject("vcnlDetails");
  vcnl["prox"] = currentData.prox;
  vcnl["al"] = currentData.ambientLight;
  vcnl["wl"] = currentData.whiteLight;

  JsonObject sht = doc.createNestedObject("shtDetails");
  sht["temp"] = currentData.temp;
  sht["rHum"] = currentData.rHum;

  JsonObject other = doc.createNestedObject("otherDetails");
  other["timeCaptured"] = timeClient.getEpochTime();
  other["userId"] = userId;
  other["currentFanState"] = fanState; // Add the current state

  String output;
  serializeJson(doc, output);
  return output;
}


// Connects to WiFi
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.print("Connecting WiFi...");
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if(++attempt > 20) { // Timeout after ~10 seconds
        Serial.println("\nFailed to connect.");
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.setTextColor(TFT_RED);
        M5.Lcd.print("WiFi Connection Failed!");
        while(true) delay(1000); // Halt on failure
    }
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  M5.Lcd.fillScreen(BLACK); // Clear connecting message
  M5.Lcd.setTextColor(WHITE, BLACK); // Reset color
}

void checkCloudCommand() {
  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Cannot check commands.");
      return;
  }

  if (URL_GCF_GET_STATE == "YOUR_GET_DEVICE_STATE_FUNCTION_URL") {
      Serial.println("ERROR: URL_GCF_GET_STATE not set!");
      return; // Don't try if URL is not configured
  }

  WiFiClientSecure client;
  client.setInsecure(); // Use secure client but disable cert verification
  HTTPClient http;

  // Construct the URL with the userId query parameter
  String requestUrl = URL_GCF_GET_STATE;
  requestUrl += "?userId=" + String(userId); // Use the device's userId

  Serial.print("Checking cloud command: ");
  Serial.println(requestUrl);

  if (http.begin(client, requestUrl)) {
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK) {
          String payload = http.getString();
          Serial.print("Command Response: ");
          Serial.println(payload);

          // Parse the JSON response
          StaticJsonDocument<512> doc; // Small doc for {"fanState": true/false}
          DeserializationError error = deserializeJson(doc, payload);

          if (error) {
              Serial.print("deserializeJson() failed: ");
              Serial.println(error.c_str());
          } else {
              if (doc.containsKey("fanState")) {
                  bool cloudFanState = doc["fanState"]; // Extract the boolean value

                  // Compare with current state and update if different
                  if (cloudFanState != fanState) {
                      Serial.printf("Cloud command received. Changing fan state from %s to %s\n",
                                    fanState ? "ON" : "OFF",
                                    cloudFanState ? "ON" : "OFF");
                      fanState = cloudFanState; // Update the internal state
                      drawFanButton();         // Update the screen display

                      // --- IMPORTANT ---
                      // TODO: Add actual hardware control logic for the fan HERE
                      // based on the new 'fanState' value.
                      // e.g., digitalWrite(FAN_PIN, fanState ? HIGH : LOW);
                      // --- END IMPORTANT ---

                  } else {
                      // Serial.println("Fan state matches cloud command.");
                  }
              } else {
                   Serial.println("JSON response missing 'fanState' key.");
              }
          }
      } else {
          Serial.printf("Cloud command check failed, HTTP code: %d Error: %s\n", httpCode, http.errorToString(httpCode).c_str());
           // Optional: read error response body
           // String errorPayload = http.getString();
           // Serial.println("Error Response Body: " + errorPayload);
      }
      http.end();
  } else {
      Serial.println("Failed to connect to command check URL.");
  }
}

// --- Setup & Main Loop ---
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Lcd.setRotation(1); // Landscape orientation
  M5.Lcd.fillScreen(BLACK);
  Serial.begin(115200);
  M5.Touch.begin(&M5.Display); // Initialize Touch

  Serial.println("M5 Core 2 Simplified Sensor Upload");

  // Initialize Sensors
  if (!vcnl4040.begin()) {
    Serial.println("VCNL4040 not found.");
    M5.Lcd.setTextColor(TFT_RED); M5.Lcd.println("VCNL4040 Error!");
    while (1) delay(1);
  } else {
     Serial.println("VCNL4040 OK.");
  }
  if (!sht4.begin()) {
    Serial.println("SHT4x not found.");
     M5.Lcd.setTextColor(TFT_RED); M5.Lcd.println("SHT4x Error!");
    while (1) delay(1);
  } else {
      Serial.println("SHT4x OK.");
  }
  sht4.setPrecision(SHT4X_HIGH_PRECISION);
  sht4.setHeater(SHT4X_NO_HEATER);


  // Connect WiFi & Start NTP
  connectWiFi();
  timeClient.begin();
  timeClient.update(); // Initial time sync

  // Draw initial screen layout
  drawScreen();

  // Initial sensor reading
  updateSensors();
  updateScreenData(); // Show initial data
}

void loop() {
  M5.update(); // Updates button states, speaker, etc.

  // Handle touch input for the fan button
  handleTouch();

  // Periodically update sensor readings and screen display
  // (Could be done less frequently than the loop runs if desired)
  updateSensors();
  updateScreenData();

  // Periodically upload data to the cloud
  if (millis() - lastUploadTime >= uploadInterval) {
    uploadData();
    lastUploadTime = millis();
  }

  // Periodically check for commands from the cloud
  if (millis() - lastCommandCheckTime >= commandCheckInterval) {
    checkCloudCommand();
    lastCommandCheckTime = millis();
}

  delay(50); // Small delay to prevent overwhelming the processor
}