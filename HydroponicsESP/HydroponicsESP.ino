// WiFi settings
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

// ── WiFi credentials ──────────────────────────────────────────
#include "passwords.h"


// ── Supabase credentials ──────────────────────────────────────
const char* supabaseUrl = "https://nyinrfhjksqaludvlvdj.supabase.co/rest/v1/readings";
const char* supabaseUrlDebug = "https://nyinrfhjksqaludvlvdj.supabase.co/rest/v1/debug";


// ── State ─────────────────────────────────────────────────────
String incomingBuffer = "";

// ── API security ──────────────────────────────────────────────


String formatCountdown(long secs) {
    char buf[6];
    if (secs >= 3600) {
        snprintf(buf, sizeof(buf), "%02d:%02d", (int)(secs/3600), (int)((secs%3600)/60));
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", (int)(secs/60), (int)(secs%60));
    }
    return String(buf);
}

void debugLog(String msg);

bool connectWiFi() {
  Serial.println("Connecting to WiFi...");
  Serial2.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    debugLog("WiFi connected: " + WiFi.localIP().toString());
    return true;
  } else {
    Serial.println("WiFi failed - will retry");
    Serial2.println("WiFi failed - will retry");
    return false;
  }
}

void sendToSupabase(String jsonPayload, bool debug=false) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi - reconnecting...");
    Serial2.println("No WiFi - reconnecting...");
    if(!connectWiFi())
      return;
  }

  HTTPClient http;
  if(debug)
    http.begin(supabaseUrlDebug);
  else
    http.begin(supabaseUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabaseKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseKey);
  http.addHeader("Prefer", "return=minimal");

  int responseCode = http.POST(jsonPayload);

  if (responseCode == 201) {
    if(debug){
      Serial.println("Supabase debug sent " + jsonPayload);
      Serial2.println("Supabase debug sent " + jsonPayload);
    }
    else
      debugLog("Supabase reading sent");
  } else {
    String msg = "Supabase error: " + String(responseCode);
    if(debug){
      Serial.println(msg);        // USB serial (when cable connected)
      Serial2.println(msg);
    }
    else
      debugLog(msg);
  }

  http.end();
}

void debugLog(String msg){
  StaticJsonDocument<392> doc;
  doc["device"] = "ESP32";
  doc["message"] = msg;

  Serial.println(msg);        // USB serial (when cable connected)
  Serial2.println(msg);       // back to Mega for monitoring

  String payload;
  serializeJson(doc, payload);
  sendToSupabase(payload, true);
}

// ── Web server ────────────────────────────────────────────────
AsyncWebServer server(80);

// ── Pump config (mirrors what Mega holds) ─────────────────────
struct RelayConfig {
public:
  long pumpOn;
  long pumpCycle;
  long lightOn;
  long lightOff;

  RelayConfig(): pumpOn(900), pumpCycle(3600), lightOn(6*3600L), lightOff(22*3600L) {};

  void save() {
    Preferences prefs;
    prefs.begin("relay", false);
    prefs.putLong("pumpOn",    pumpOn);
    prefs.putLong("pumpCycle", pumpCycle);
    prefs.putLong("lightOn",   lightOn);
    prefs.putLong("lightOff",  lightOff);
    prefs.end();

    debugLog("Saved config settings: Pump ON=" + String(pumpOn/60) +
             " OFF=" + String((pumpCycle-pumpOn)/60) +
             " | Light ON=" + formatCountdown(lightOn) +
             " OFF=" + formatCountdown(lightOff));
  }

  void load() {
    Preferences prefs;
    prefs.begin("relay", true);
    pumpOn  = prefs.getLong("pumpOn",   pumpOn);
    pumpCycle = prefs.getLong("pumpCycle",  pumpCycle);
    lightOn  = prefs.getLong("lightOn",  lightOn);
    lightOff = prefs.getLong("lightOff", lightOff);
    prefs.end();

    Serial2.println("{\"cmd\":\"getConfig\"}");
    debugLog("Requested config from Mega");
  }

  void sendConfigToMega() {
    StaticJsonDocument<192> cmd;
    cmd["cmd"]       = "setRelay";
    cmd["pumpOn"]    = pumpOn;
    cmd["pumpCycle"] = pumpCycle;
    cmd["lightOn"]   = lightOn;
    cmd["lightOff"]  = lightOff;
    String out;
    serializeJson(cmd, out);
    Serial2.println(out);
    debugLog("Sent to Mega: " + out);
  }

  void setConfigFromMega(const JsonDocument &doc){
    if(doc.containsKey("pumpOn") && doc.containsKey("pumpCycle") && doc.containsKey("lightOn") && doc.containsKey("lightOff")){
      long pumpOnTemp = doc["pumpOn"].as<long>();
      long pumpCycleTemp = doc["pumpCycle"].as<long>();
      long lightOnTemp = doc["lightOn"].as<long>();
      long lightOffTemp = doc["lightOff"].as<long>();

      String out;
      serializeJson(doc, out);
      debugLog("Received relay config from Mega: " + out);

      if(pumpOnTemp != pumpOn || pumpCycleTemp != pumpCycle || lightOnTemp != lightOn || lightOffTemp != lightOff){
        pumpOn = pumpOnTemp;
        pumpCycle = pumpCycleTemp;
        lightOn = lightOnTemp;
        lightOff = lightOffTemp;
        save();
      }
    }
    else{
      String out;
      serializeJson(doc, out);
      debugLog("Relay config from Mega is missing fields: " + out);
    }
  }
} relayConfig;

// ── Rate limiting (brute-force protection) ────────────────────
struct RateLimit {
  uint32_t windowStart = 0;
  int      failCount   = 0;
  bool     blocked     = false;
} rateLimit;

const int    MAX_FAILS      = 5;
const uint32_t BLOCK_MS     = 60000;  // 60 seconds

// ── Auth helper ───────────────────────────────────────────────
bool isAuthorized(AsyncWebServerRequest* req) {
  // Check rate limit
  uint32_t now = millis();
  if (rateLimit.blocked) {
    if (now - rateLimit.windowStart < BLOCK_MS) return false;
    rateLimit.blocked   = false;
    rateLimit.failCount = 0;
  }

  bool ok = req->hasHeader("X-API-Key") &&
            req->getHeader("X-API-Key")->value() == String(API_KEY);

  if (!ok) {
    rateLimit.failCount++;
    if (rateLimit.failCount == 1) rateLimit.windowStart = now;
    if (rateLimit.failCount >= MAX_FAILS) {
      rateLimit.blocked = true;
      debugLog("Auth: too many failures, blocking for 60s");
    }
    return false;
  }

  rateLimit.failCount = 0;
  return true;
}

// ── Setup web server routes ────────────────────────────────────
void setupServer() {
  // GET /api/relay — read current config
  server.on("/api/relay", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) {
      req->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    StaticJsonDocument<192> doc;
    doc["pumpOnMinutes"]  = (long) relayConfig.pumpOn/60;
    doc["pumpOffMinutes"] = (long) (relayConfig.pumpCycle - relayConfig.pumpOn)/60;
    doc["lightOnMinutes"] = (long) relayConfig.lightOn/60;
    doc["lightOffMinutes"] = (long) relayConfig.lightOff/60;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // POST /api/relay — update pump config
  // Body: {"pumpOnMinutes": 15, "pumpOffMinutes": 45, "lightOnMinutes": 45, "lightOffMinutes": 45}
  server.on("/api/relay", HTTP_POST,
    [](AsyncWebServerRequest* req) {},   // headers handler (required)
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      if (!isAuthorized(req)) {
        req->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }

      StaticJsonDocument<192> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      bool changed = false;
      if (doc.containsKey("pumpOnMinutes")) {
        long v = doc["pumpOnMinutes"].as<long>();
        if (v >= 1 && v <= 120) {
          relayConfig.pumpCycle = relayConfig.pumpCycle - relayConfig.pumpOn + 60*v;
          relayConfig.pumpOn = 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("pumpOffMinutes")) {
        long v = doc["pumpOffMinutes"].as<long>();
        if (v >= 1 && v <= 240) {
          relayConfig.pumpCycle = relayConfig.pumpOn + 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("lightOnMinutes")) {
        long v = doc["lightOnMinutes"].as<long>();
        if (v >= 0 && v < 1440) {
          relayConfig.lightOn = 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("lightOffMinutes")) {
        long v = doc["lightOffMinutes"].as<long>();
        if (v >= 0 && v < 1440) {
          relayConfig.lightOff = 60*v;
          changed = true;
        }
      }

      if (changed) {
        relayConfig.sendConfigToMega();
        relayConfig.save();
        StaticJsonDocument<192> resp;
        resp["status"]     = "ok";
        resp["pumpOnMinutes"]  = (long) relayConfig.pumpOn/60;
        resp["pumpCycleMinutes"] = (long) (relayConfig.pumpCycle - relayConfig.pumpOn)/60;
        resp["lightOnMinutes"] = (long) relayConfig.lightOn/60;
        resp["lightOffMinutes"] = (long) relayConfig.lightOff/60;
        String respStr;
        serializeJson(resp, respStr);
        req->send(200, "application/json", respStr);
      } else {
        req->send(400, "application/json", "{\"error\":\"No valid fields or values out of range\"}");
      }
    }
  );

  // 404 handler
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "application/json", "{\"error\":\"Not found\"}");
  });

  server.begin();
  debugLog("Web server started");
}

void setup() {
  Serial.begin(115200);          // USB debug
  Serial2.begin(115200, SERIAL_8N1, 16, 17);         // UART2: GPIO16 (RX) / GPIO17 (TX) ← Mega

  Serial.println("ESP32 booting...");
  Serial2.println("ESP32 booting...");
  connectWiFi();
  debugLog("ESP32 booted");

  if (MDNS.begin("hydrotower")) {
    debugLog("mDNS: hydrotower.local");
  }

  relayConfig.load();
  setupServer();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  while (Serial2.available()) {
    char c = Serial2.read();
    if(incomingBuffer.length() == 0 && c == '\0') continue;
    if (c == '\n') {
      incomingBuffer.trim();
      if (incomingBuffer.length() > 0) {
        // Parse JSON from Mega
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, incomingBuffer);

        if(err){
          debugLog("From Mega: " + incomingBuffer + ", JSON parse error: " + String(err.c_str()));
        }
        else if(!doc.containsKey("cmd")) {
          debugLog("From Mega: " + incomingBuffer + ", cmd not found");
        }
        else{
          String cmd = doc["cmd"].as<String>();
          doc.remove("cmd");
          if(cmd == "log"){
            doc["device"] = "Mega";

            String payload;
            serializeJson(doc, payload);
            sendToSupabase(payload, true);
          }
          else if(cmd == "configReport"){
            relayConfig.setConfigFromMega(doc);
          }
          else if(cmd == "reading"){
            String payload;
            serializeJson(doc, payload);
            sendToSupabase(payload, false);
          }
          else{
            debugLog("From Mega: " + incomingBuffer + ", unrecognized cmd command");
          }
        }
      }
      incomingBuffer = "";
    } else {
      if (incomingBuffer.length() < 512)
        incomingBuffer += c;
      else
        incomingBuffer = "";
    }
  }
}