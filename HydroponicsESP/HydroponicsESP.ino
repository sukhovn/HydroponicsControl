// WiFi settings
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

#include "html_code.h"
#include "passwords.h"

// ── Supabase URLs ──────────────────────────────────────
const char* supabaseUrl = "https://nyinrfhjksqaludvlvdj.supabase.co/rest/v1/readings";
const char* supabaseUrlDebug = "https://nyinrfhjksqaludvlvdj.supabase.co/rest/v1/debug";

// ── State ─────────────────────────────────────────────────────
String incomingBuffer = "";

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

class LoggingConfig {
public:
  bool enabled;

  LoggingConfig(): enabled(true) {};

  void load(){
    Preferences prefs;
    prefs.begin("logging", true);
    enabled = prefs.getBool("enabled", enabled);
    prefs.end();
  }

  void update(bool _enabled) {
    enabled = true;
    debugLog(String("Logging ") + (_enabled ? "ON" : "OFF"));

    enabled = _enabled;
    Preferences prefs;
    prefs.begin("logging", false);
    prefs.putBool("enabled", enabled);
    prefs.end();
  }
} loggingConfig;

void debugLog(String msg){
  StaticJsonDocument<392> doc;
  doc["device"] = "ESP32";
  doc["message"] = msg;

  Serial.println(msg);        // USB serial (when cable connected)
  Serial2.println(msg);       // back to Mega for monitoring

  if(loggingConfig.enabled){
    String payload;
    serializeJson(doc, payload);
    sendToSupabase(payload, true);
  }
}

// ── Web server ────────────────────────────────────────────────
AsyncWebServer server(80);

// ── Controller config (mirrors what Mega holds) ─────────────────────
struct ControlConfig {
public:
  long pumpOn;
  long pumpCycle;
  long lightOn;
  long lightOff;
  long readingInt;

  ControlConfig(): pumpOn(900), pumpCycle(3600), lightOn(6*3600L), lightOff(22*3600L), readingInt(60) {};

  void save() {
    Preferences prefs;
    prefs.begin("control", false);
    prefs.putLong("pumpOn",     pumpOn);
    prefs.putLong("pumpCycle",  pumpCycle);
    prefs.putLong("lightOn",    lightOn);
    prefs.putLong("lightOff",   lightOff);
    prefs.putLong("readingInt", readingInt);
    prefs.end();

    debugLog("Saved config settings: Pump ON=" + String(pumpOn/60) +
             " OFF=" + String((pumpCycle-pumpOn)/60) +
             " | Light ON=" + formatCountdown(lightOn) +
             " OFF=" + formatCountdown(lightOff) + " | ReadingInt=" + String(readingInt));
  }

  void load() {
    Preferences prefs;
    prefs.begin("control", true);
    pumpOn  = prefs.getLong("pumpOn",   pumpOn);
    pumpCycle = prefs.getLong("pumpCycle",  pumpCycle);
    lightOn  = prefs.getLong("lightOn",  lightOn);
    lightOff = prefs.getLong("lightOff", lightOff);
    readingInt = prefs.getLong("readingInt", readingInt);
    prefs.end();

    Serial2.println("{\"cmd\":\"getConfig\"}");
    debugLog("Requested config from Mega");
  }

  void sendConfigToMega() {
    StaticJsonDocument<192> cmd;
    cmd["cmd"]       = "setConfig";
    cmd["pumpOn"]    = pumpOn;
    cmd["pumpCycle"] = pumpCycle;
    cmd["lightOn"]   = lightOn;
    cmd["lightOff"]  = lightOff;
    cmd["readingInt"] = readingInt;
    String out;
    serializeJson(cmd, out);
    Serial2.println(out);
    debugLog("Sent to Mega: " + out);
  }

  void setConfigFromMega(const JsonDocument &doc){
    if(doc.containsKey("pumpOn") && doc.containsKey("pumpCycle") &&
       doc.containsKey("lightOn") && doc.containsKey("lightOff") && doc.containsKey("readingInt")){
      long pumpOnTemp = doc["pumpOn"].as<long>();
      long pumpCycleTemp = doc["pumpCycle"].as<long>();
      long lightOnTemp = doc["lightOn"].as<long>();
      long lightOffTemp = doc["lightOff"].as<long>();
      long readingIntTemp = doc["readingInt"].as<long>();

      String out;
      serializeJson(doc, out);
      debugLog("Received controller config from Mega: " + out);

      if(pumpOnTemp != pumpOn || pumpCycleTemp != pumpCycle ||
         lightOnTemp != lightOn || lightOffTemp != lightOff || readingIntTemp != readingInt){
        pumpOn = pumpOnTemp;
        pumpCycle = pumpCycleTemp;
        lightOn = lightOnTemp;
        lightOff = lightOffTemp;
        readingInt = readingIntTemp;
        save();
      }
    }
    else{
      String out;
      serializeJson(doc, out);
      debugLog("Controller config from Mega is missing fields: " + out);
    }
  }
} controlConfig;

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
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "X-API-Key, Content-Type");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");

  // GET / — serve dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  // GET /api/control — read controller config
  server.on("/api/control", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) {
      req->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    StaticJsonDocument<192> doc;
    doc["pumpOnMinutes"]  = (long) controlConfig.pumpOn/60;
    doc["pumpOffMinutes"] = (long) (controlConfig.pumpCycle - controlConfig.pumpOn)/60;
    doc["lightOnMinutes"] = (long) controlConfig.lightOn/60;
    doc["lightOffMinutes"] = (long) controlConfig.lightOff/60;
    doc["readingInterval"] = (long) controlConfig.readingInt;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // POST /api/control — update control config
  // Body: {"pumpOnMinutes": 15, "pumpOffMinutes": 45, "lightOnMinutes": 360, "lightOffMinutes": 1320, "readingInterval": 60}
  server.on("/api/control", HTTP_POST,
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
          controlConfig.pumpCycle = controlConfig.pumpCycle - controlConfig.pumpOn + 60*v;
          controlConfig.pumpOn = 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("pumpOffMinutes")) {
        long v = doc["pumpOffMinutes"].as<long>();
        if (v >= 1 && v <= 240) {
          controlConfig.pumpCycle = controlConfig.pumpOn + 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("lightOnMinutes")) {
        long v = doc["lightOnMinutes"].as<long>();
        if (v >= 0 && v < 1440) {
          controlConfig.lightOn = 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("lightOffMinutes")) {
        long v = doc["lightOffMinutes"].as<long>();
        if (v >= 0 && v < 1440) {
          controlConfig.lightOff = 60*v;
          changed = true;
        }
      }
      if (doc.containsKey("readingInterval")) {
        long v = doc["readingInterval"].as<long>();
        if (v >= 5) {
          controlConfig.readingInt = v;
          changed = true;
        }
      }

      if(changed){
        controlConfig.sendConfigToMega();
        controlConfig.save();
        StaticJsonDocument<192> resp;
        resp["status"]     = "ok";
        resp["pumpOnMinutes"]  = (long) controlConfig.pumpOn/60;
        resp["pumpCycleMinutes"] = (long) (controlConfig.pumpCycle - controlConfig.pumpOn)/60;
        resp["lightOnMinutes"] = (long) controlConfig.lightOn/60;
        resp["lightOffMinutes"] = (long) controlConfig.lightOff/60;
        resp["readingInterval"] = (long) controlConfig.readingInt;
        String respStr;
        serializeJson(resp, respStr);
        req->send(200, "application/json", respStr);
      } else {
        req->send(400, "application/json", "{\"error\":\"No valid fields or values out of range\"}");
      }
    }
  );

  // POST /api/logging — switch Supabase logging on/ogg
  // Body: {"enabled": true}
  server.on("/api/logging", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      if (!isAuthorized(req)) { req->send(401); return; }

      StaticJsonDocument<64> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if(err){
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if(!doc["enabled"].is<bool>()){
        req->send(400, "application/json", "{\"error\":\"'enabled' must be a boolean\"}");
        return;
      }

      loggingConfig.update(doc["enabled"].as<bool>());

      req->send(200, "application/json",
        loggingConfig.enabled ? "{\"logging\":\"on\"}" : "{\"logging\":\"off\"}");
    }
  );

  // POST /api/manual — manual pump/lights override
  // Body: {"manual": true, "pump": true, "light": true}
  server.on("/api/manual", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      if (!isAuthorized(req)) { req->send(401); return; }

      StaticJsonDocument<192> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if(err){
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if(!doc.containsKey("manual") || !doc["manual"].is<bool>()){
        req->send(400, "application/json", "{\"error\":\"No boolean 'manual' found\"}");
        return;
      }

      StaticJsonDocument<192> cmd;
      bool manual = doc["manual"].as<bool>();
      cmd["manual"] = manual;

      if(manual){
        bool no_manual_command = true;
        if(doc.containsKey("pump")){
          no_manual_command = false;
          if(!doc["pump"].is<bool>()){
            req->send(400, "application/json", "{\"error\":\"'pump' must be a boolean\"}");
            return;
          }
          cmd["pump"] = doc["pump"].as<bool>();
        }
        if(doc.containsKey("light")){
          no_manual_command = false;
          if(!doc["light"].is<bool>()){
            req->send(400, "application/json", "{\"error\":\"'light' must be a boolean\"}");
            return;
          }
          cmd["light"] = doc["light"].as<bool>();
        }
        if(no_manual_command){
          req->send(400, "application/json", "{\"error\":\"Manual override command not found\"}");
          return;
        }
      }

      String cmd_out, req_out;
      serializeJson(cmd, req_out);
      cmd["cmd"] = "override";
      serializeJson(cmd, cmd_out);

      Serial2.println(cmd_out);
      debugLog("Sent to Mega: " + cmd_out);

      req->send(200, "application/json", req_out);
    }
  );

  // 404 handler
  server.onNotFound([](AsyncWebServerRequest* req) {
    if (req->method() == HTTP_OPTIONS) {
      req->send(200);
    } else {
      req->send(404, "application/json", "{\"error\":\"Not found\"}");
    }
  });

  server.begin();
  debugLog("Web server started");
}

void setup() {
  Serial.begin(115200);          // USB debug
  Serial2.begin(115200, SERIAL_8N1, 16, 17);         // UART2: GPIO16 (RX) / GPIO17 (TX) ← Mega

  Serial.println("ESP32 booting...");
  Serial2.println("ESP32 booting...");

  loggingConfig.load();
  connectWiFi();
  debugLog("ESP32 booted");

  if (MDNS.begin("hydrotower")) {
    debugLog("mDNS: hydrotower.local");
  }

  controlConfig.load();
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

            if(loggingConfig.enabled){
              String payload;
              serializeJson(doc, payload);
              sendToSupabase(payload, true);
            }
          }
          else if(cmd == "configReport"){
            controlConfig.setConfigFromMega(doc);
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