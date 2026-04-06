// WiFi settings
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── WiFi credentials ──────────────────────────────────────────
#include "passwords.h"


// ── Supabase credentials ──────────────────────────────────────
const char* supabaseUrl = "https://nyinrfhjksqaludvlvdj.supabase.co/rest/v1/readings";


// ── State ─────────────────────────────────────────────────────
String incomingBuffer = "";

void debugLog(String msg) {
  Serial.println(msg);        // USB serial (when cable connected)
  Serial2.println(msg);       // back to Mega for monitoring
}

bool connectWiFi() {
  debugLog("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    debugLog("WiFi connected: " + WiFi.localIP().toString());
    return false;
  } else {
    debugLog("WiFi failed - will retry");
    return true;
  }
}

void sendToSupabase(String jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    debugLog("No WiFi - reconnecting...");
    if(connectWiFi())
      return;
  }

  HTTPClient http;
  http.begin(supabaseUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabaseKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseKey);
  http.addHeader("Prefer", "return=minimal");

  int responseCode = http.POST(jsonPayload);

  if (responseCode == 201) {
    debugLog("Supabase OK");
  } else {
    debugLog("Supabase error: " + String(responseCode));
  }

  http.end();
}

void setup() {
  Serial.begin(115200);          // USB debug
  Serial2.begin(115200, SERIAL_8N1, 16, 17);         // UART2: GPIO16 (RX) / GPIO17 (TX) ← Mega

  debugLog("ESP32 booting...");
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      incomingBuffer.trim();
      if (incomingBuffer.length() > 0) {
        debugLog("From Mega: " + incomingBuffer);

        // Parse JSON from Mega
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, incomingBuffer);

        if (!err) {
          sendToSupabase(incomingBuffer);
        } else {
          debugLog("JSON parse error: " + String(err.c_str()));
        }
      }
      incomingBuffer = "";
    } else {
      incomingBuffer += c;
    }
  }
}

// int i = 0;

// void setup() {
//   Serial.begin(115200);
//   Serial2.begin(115200, SERIAL_8N1, 16, 17);

//   Serial.println("OK");
//   Serial2.println("OK");
// }

// void loop(){
//   Serial.print("Checking connection ");
//   Serial.println(i);
//   Serial2.print("Checking connection ");
//   Serial2.println(i);
//   ++i;
//   delay(5000);
// }