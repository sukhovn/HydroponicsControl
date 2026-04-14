#include <RTClib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h>
#include <EEPROM.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

RTC_DS1307 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// EEPROM address to store timer config
#define EEPROM_ADDR 0
#define EEPROM_MAGIC 0xAB  // sentinel to detect valid data

struct RelayConfig{
  uint8_t magic;
  long pumpOn;
  long pumpCycle;
  long lightOn;
  long lightOff;
};

// Button and pump leg shift parameters
#define BUTTON_1 28
#define LEG_SHIFT 30

// One wire bus
#define ONE_WIRE_BUS 4
// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature tempSensor(&oneWire);

// Echo distance parameters
#define TRIG_PIN 5
#define ECHO_PIN 6
#define TIMEOUT_US 30000

// Temp variables
uint8_t last_second;
char line0[17];
char line1[17];
char serialBuffer[20];

StaticJsonDocument<384> doc;
char serialESPBuffer[384];

// Time functions
long timeFromHMS(long hour, long minute, long second){
  return 3600 * hour + 60 * minute + second;
}

void formatCountdown(long secs, char* buf, int bufSize) {
  if (secs >= 3600) {
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    snprintf(buf, bufSize, "%02d:%02d", h, m);
  } else {
    int m = secs / 60;
    int s = secs % 60;
    snprintf(buf, bufSize, "%02d:%02d", m, s);
  }
}

// Valid float check
bool isValidFloat(const char* buf) {
  if (buf == nullptr || buf[0] == '\0') return false;
  bool hasDot = false, hasDigit = false;
  int i = 0;
  if (buf[0] == '-') i++;
  for (; buf[i] != '\0'; i++) {
    if (buf[i] == '.' && !hasDot) { hasDot = true; continue; }
    if (buf[i] < '0' || buf[i] > '9') return false;
    hasDigit = true;
  }
  return hasDigit;
}

// Reading water level
float readLevel() {
  // Send trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(50);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo pulse width
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, TIMEOUT_US);

  if (duration == 0) return -1;

  return (duration * 0.0343f) / 2.0f;
}

// Storing water level to DS1307 SRAM
#define DS1307_ADDR 0x68
#define SRAM_FLAG_ADDR  0 // Flag (1 = valid data saved, 0 = no data saved)
#define SRAM_FLOAT_ADDR 1 // Bytes 1-4 for the float

// Write a single byte to DS1307 SRAM, SRAM starts at register 0x08
void ds1307_write_byte(uint8_t sram_offset, uint8_t value) {
  Wire.beginTransmission(DS1307_ADDR);
  Wire.write(0x08 + sram_offset);  // point to SRAM register
  Wire.write(value);
  Wire.endTransmission();
}

// Read a single byte from DS1307 SRAM
uint8_t ds1307_read_byte(uint8_t sram_offset) {
  Wire.beginTransmission(DS1307_ADDR);
  Wire.write(0x08 + sram_offset);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t) DS1307_ADDR, (uint8_t) 1);
  return Wire.read();
}

// Store a float in DS1307 SRAM (uses 4 bytes starting at sram_offset)
void sram_write_float(uint8_t sram_offset, float value) {
  uint8_t *p = (uint8_t*)&value;  // treat float as 4 raw bytes
  for (uint8_t i = 0; i < 4; i++) {
    ds1307_write_byte(sram_offset + i, p[i]);
  }
}

// Read a float from DS1307 SRAM
float sram_read_float(uint8_t sram_offset) {
  float value;
  uint8_t *p = (uint8_t*)&value;
  for (uint8_t i = 0; i < 4; i++) {
    p[i] = ds1307_read_byte(sram_offset + i);
  }
  return value;
}

// Sending debugs to ESP32
void debugLog(const char* msg) {
  Serial.println(msg);
  int len = snprintf(serialESPBuffer, sizeof(serialESPBuffer), "{\"cmd\":\"log\",\"message\":\"%s\"}", msg);
  if (len > 0 && len < sizeof(serialESPBuffer)) {
    Serial1.println(serialESPBuffer);
  }
}

// Timer code
class RelayTimer{
  long pumpOn;
  long pumpCycle;
  long lightOn;
  long lightOff;

  bool pumpIsOn;
  bool lightIsOn;
  long totalSeconds;
  long cycleSeconds;

  bool pumpOverride;
  bool lightOverride;

  void switchPumpOn(){
    digitalWrite(LEG_SHIFT, LOW);
    if(!pumpIsOn){
      lcd.init();
      lcd.backlight();
      pumpIsOn = true;
      debugLog("Pump switched on");
    }
  }

  void switchPumpOff(){
    digitalWrite(LEG_SHIFT, HIGH);
    if(pumpIsOn){
      lcd.init();
      lcd.backlight();
      pumpIsOn = false;
      debugLog("Pump switched off");
    }
  }

  void switchLightOn(){
    digitalWrite(LEG_SHIFT+1, LOW);
    if(!lightIsOn){
      lcd.init();
      lcd.backlight();
      lightIsOn = true;
      debugLog("Light switched on");
    }
  }

  void switchLightOff(){
    digitalWrite(LEG_SHIFT+1, HIGH);
    if(lightIsOn){
      lcd.init();
      lcd.backlight();
      lightIsOn = false;
      debugLog("Light switched off");
    }
  }

  void saveRelayConfig(){
    RelayConfig cfg = {EEPROM_MAGIC, pumpOn, pumpCycle, lightOn, lightOff};
    EEPROM.put(EEPROM_ADDR, cfg);
  }

public:
  RelayTimer(long _pumpOn, long _pumpOff, long _lightOn, long _lightOff):
    pumpOn(_pumpOn), pumpCycle(_pumpOn+_pumpOff), lightOn(_lightOn), lightOff(_lightOff),
    pumpIsOn(false), lightIsOn(false), totalSeconds(0), cycleSeconds(0),
    pumpOverride(false), lightOverride(false) {};

  void updateRelayConfig(long _pumpOn, long _pumpCycle, long _lightOn, long _lightOff){
    pumpOn = _pumpOn; pumpCycle = _pumpCycle; lightOn = _lightOn; lightOff = _lightOff;
    saveRelayConfig();

    char lightOnBuf[6], lightOffBuf[6];
    formatCountdown(lightOn, lightOnBuf, sizeof(lightOnBuf));
    formatCountdown(lightOff, lightOffBuf, sizeof(lightOffBuf));

    char localBuf[80];
    snprintf(localBuf, sizeof(localBuf), "Relay config updated: Pump ON=%ld OFF=%ld | Light ON=%s OFF=%s",
             pumpOn/60, (pumpCycle - pumpOn)/60, lightOnBuf, lightOffBuf);
    debugLog(localBuf);
  }

  void sendConfigReport() {
    doc.clear();
    doc["cmd"]       = "configReport";
    doc["pumpOn"]    = pumpOn;
    doc["pumpCycle"] = pumpCycle;
    doc["lightOn"]   = lightOn;
    doc["lightOff"]  = lightOff;

    serializeJson(doc, serialESPBuffer, sizeof(serialESPBuffer));
    Serial1.println(serialESPBuffer);

    Serial.print("Sent configReport to ESP32: ");
    Serial.println(serialESPBuffer);
  }

  bool isPumpOn() const {return pumpIsOn;}
  bool isLightOn() const {return lightIsOn;}
  long timeAfterPumpOff() const {
    if(pumpIsOn)
      return 0;
    else
      return cycleSeconds - pumpOn;
  }

  void setup(const DateTime &dt){
    // Clecking if relay config is in EEPROM and loading it
    RelayConfig cfg;
    EEPROM.get(EEPROM_ADDR, cfg);

    char localBuf[80];
    if(cfg.magic == EEPROM_MAGIC){
      pumpOn = cfg.pumpOn;
      pumpCycle = cfg.pumpCycle;
      lightOn = cfg.lightOn;
      lightOff = cfg.lightOff;

      char lightOnBuf[6], lightOffBuf[6];
      formatCountdown(lightOn, lightOnBuf, sizeof(lightOnBuf));
      formatCountdown(lightOff, lightOffBuf, sizeof(lightOffBuf));

      snprintf(localBuf, sizeof(localBuf), "Loaded from EEPROM: Pump ON=%ld OFF=%ld | Light ON=%s OFF=%s",
               pumpOn/60, (pumpCycle - pumpOn)/60, lightOnBuf, lightOffBuf);
      debugLog(localBuf);
    }
    else{
      debugLog("No valid EEPROM data, using defaults");
      saveRelayConfig();
    }

    // Setting up initial relay configuration
    totalSeconds = timeFromHMS(dt.hour(), dt.minute(), dt.second());
    cycleSeconds = totalSeconds % pumpCycle;

    pinMode(LEG_SHIFT, OUTPUT);
    if(cycleSeconds < pumpOn){
      digitalWrite(LEG_SHIFT, LOW);
      pumpIsOn = true;
    }
    else{
      digitalWrite(LEG_SHIFT, HIGH);
      pumpIsOn = false;
    }

    pinMode(LEG_SHIFT+1, OUTPUT);
    if(totalSeconds >= lightOn && totalSeconds < lightOff){
      digitalWrite(LEG_SHIFT+1, LOW);
      lightIsOn = true;
    }
    else{
      digitalWrite(LEG_SHIFT+1, HIGH);
      lightIsOn = false;
    }

    snprintf(localBuf, sizeof(localBuf), "At reboot Pump %s Light %s", pumpIsOn ? "ON" : "OFF", lightIsOn ? "ON" : "OFF");
    debugLog(localBuf);
  }

  void update(const DateTime &dt){
    totalSeconds = timeFromHMS(dt.hour(), dt.minute(), dt.second());
    cycleSeconds = totalSeconds % pumpCycle;

    if(!pumpOverride){
      if(cycleSeconds < pumpOn)
        switchPumpOn();
      else
        switchPumpOff();
    }

    if(!lightOverride){
      if(totalSeconds >= lightOn && totalSeconds < lightOff)
        switchLightOn();
      else
        switchLightOff();
    }
  }

  void printStatus(){
    char cntBuf[6];

    memset(line0, ' ', 16);
    line0[16] = '\0'; 
    if (cycleSeconds < pumpOn) {
      memcpy(line0, "Pump on:  ", 10);
      formatCountdown(pumpOn - cycleSeconds, cntBuf, sizeof(cntBuf));
      memcpy(line0 + 10, cntBuf, 5);
    } else {
      memcpy(line0, "Pump off: ", 10);
      formatCountdown(pumpCycle - cycleSeconds, cntBuf, sizeof(cntBuf));
      memcpy(line0 + 10, cntBuf, 5);
    }

    memset(line1, ' ', 16);
    line1[16] = '\0'; 
    if(totalSeconds >= lightOn && totalSeconds < lightOff){
      memcpy(line1, "Light off: ", 11);
      formatCountdown(lightOff - totalSeconds, cntBuf, sizeof(cntBuf));
      memcpy(line1 + 11, cntBuf, 5);
    }
    else{
      memcpy(line1, "Light on:  ", 11);
      long t = lightOn < totalSeconds ? 86400 + lightOn - totalSeconds : lightOn - totalSeconds;
      formatCountdown(t, cntBuf, sizeof(cntBuf));
      memcpy(line1 + 11, cntBuf, 5);
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcd.setCursor(0, 1);
    lcd.print(line1);
  }

  void overridePump(bool pump){
    pumpOverride = true;
    debugLog("Pump override ON");
    if(pump)
      switchPumpOn();
    else
      switchPumpOff();
  }

  void overrideLight(bool light){
    lightOverride = true;
    debugLog("Light override ON");
    if(light)
      switchLightOn();
    else
      switchLightOff();
  }

  void overrideOff(){
    pumpOverride = false;
    lightOverride = false;
    debugLog("Relay override OFF");
  }

} relay_timer(900, 2700, timeFromHMS(6,0,0), timeFromHMS(22,0,0));

// Measurement code
class Measurements{
  long databaseSaveInverval;
  long levelMeasureCooldown;
  uint32_t lastSave;

  float waterTemp;
  float waterLevel;
  float ph;
  float ec;

  bool waterLevelFrozen;

  void sendReading(){
    doc.clear();
    doc["cmd"] = "reading";
    doc["temp"] = waterTemp;
    doc["ph"] = ph;
    doc["ec"] = ec;
    doc["wlevel"] = waterLevel;
    doc["pump"] = relay_timer.isPumpOn();
    doc["light"] = relay_timer.isLightOn();

    serializeJson(doc, serialESPBuffer, sizeof(serialESPBuffer));
    Serial1.println(serialESPBuffer);   // ESP32 via TX1/RX1 (pins 18/19)

    Serial.print(F("Sent: "));
    Serial.println(serialESPBuffer);
  }

public:
  Measurements(long _databaseSaveInverval, long _levelMeasureCooldown): databaseSaveInverval(_databaseSaveInverval),
    levelMeasureCooldown(_levelMeasureCooldown), lastSave(0), waterTemp(0), waterLevel(-1.0f), ph(7), ec(1),
    waterLevelFrozen(false) {}

  void setLastMeasurement(const DateTime &dt){
    lastSave = dt.unixtime();
  }

  void recoverWaterLevel(){
    if(ds1307_read_byte(SRAM_FLAG_ADDR) == 1){
      waterLevel = sram_read_float(SRAM_FLOAT_ADDR);
      waterLevelFrozen = true;
    }
    else
      waterLevelFrozen = false;
  }

  void update(const DateTime &dt){
    tempSensor.requestTemperatures();
    waterTemp = tempSensor.getTempCByIndex(0);
    if(relay_timer.timeAfterPumpOff() >= levelMeasureCooldown){
      waterLevel = 24.6 - readLevel(); // Bucket depth with no water as measured by ultrasound is 24.6
      if(waterLevelFrozen){
        ds1307_write_byte(SRAM_FLAG_ADDR, 0);
        waterLevelFrozen = false;
      }
    }
    else if(!waterLevelFrozen){
      ds1307_write_byte(SRAM_FLAG_ADDR, 1);
      sram_write_float(SRAM_FLOAT_ADDR, waterLevel);
      waterLevelFrozen = true;
    }

    uint32_t timestamp = dt.unixtime();
    if(timestamp - lastSave >= databaseSaveInverval){
      sendReading();
      lastSave = timestamp;
    }
  }

  void serialCheck(){
    if(Serial2.available()){
      int len = Serial2.readBytesUntil('\r', serialBuffer, sizeof(serialBuffer)-1);
      serialBuffer[len] = '\0';
      if(isValidFloat(serialBuffer)){
        float val = atof(serialBuffer);
        if (val >= 0.0 && val <= 14.0) ph = val;
      }
    }

    if (Serial3.available()) {
      int len = Serial3.readBytesUntil('\r', serialBuffer, sizeof(serialBuffer) - 1);
      serialBuffer[len] = '\0';
      if(isValidFloat(serialBuffer)){
        float val = atof(serialBuffer);
        if(val >= 0.0) ec = val/1000;
      }
    }
  }

  void printStatus(){
    char buffer1[6], buffer2[6];

    memset(line0, ' ', 16);
    line0[16] = '\0'; 
    dtostrf(ph, 5, 2, buffer1);
    dtostrf(ec, 5, 2, buffer2);
    snprintf(line0, sizeof(line0), "pH%s EC%s", buffer1, buffer2);

    memset(line1, ' ', 16);
    line1[16] = '\0'; 
    dtostrf(waterTemp, 5, 1, buffer1);
    dtostrf(waterLevel, 6, 1, buffer2);
    snprintf(line1, sizeof(line1), "T%s L%s", buffer1, buffer2);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcd.setCursor(0, 1);
    lcd.print(line1);
  }
} measurements(60, 30);

// ESP32 receiver function
void checkESP32Messages() {
  while (Serial1.available()) {
    int len = Serial1.readBytesUntil('\n', serialESPBuffer, sizeof(serialESPBuffer)-1);
    if (len > 0 && serialESPBuffer[len-1] == '\r') len--;  // trim trailing \r
    serialESPBuffer[len] = '\0';

    DeserializationError err = deserializeJson(doc, serialESPBuffer);
    if(!err){
      if(doc.containsKey("cmd")){
        const char* cmd = doc["cmd"];
        if(strcmp(cmd, "setRelay") == 0){
          if(doc.containsKey("pumpOn") && doc.containsKey("pumpCycle") && doc.containsKey("lightOn") && doc.containsKey("lightOff")){
            relay_timer.updateRelayConfig(doc["pumpOn"].as<long>(), doc["pumpCycle"].as<long>(),
                                          doc["lightOn"].as<long>(), doc["lightOff"].as<long>());
          }
          else{
            debugLog("setRelay missing fields");
          }
        }
        else if(strcmp(cmd, "getConfig") == 0){
          relay_timer.sendConfigReport();
        }
        else if(strcmp(cmd, "override") == 0){
          if(doc.containsKey("manual")){
            if(doc["manual"].as<bool>()){
              bool no_manual_command = true;
              if(doc.containsKey("pump")){
                no_manual_command = false;
                relay_timer.overridePump(doc["pump"].as<bool>());
              }
              if(doc.containsKey("light")){
                no_manual_command = false;
                relay_timer.overrideLight(doc["light"].as<bool>());
              }
              if(no_manual_command)
                debugLog("override missing override command");
            }
            else{
              relay_timer.overrideOff();
            }
          }
          else{
            debugLog("override missing manual");
          }
        }
        else{
          char localBuf[80];
          snprintf(localBuf, sizeof(localBuf), "Unknown cmd: %s", cmd);
          debugLog(localBuf);
        }
      }
      else{
        debugLog(serialESPBuffer);
      }
    }

    if(len > 0){
      Serial.print(F("ESP32: "));
      Serial.println(serialESPBuffer);
    }
  }
}

void setup() {
  MCUSR = 0;
  wdt_disable();

  // Debug serial
  Serial.begin(115200);
  // ESP32 serial
  Serial1.begin(115200);
  delay(1000);

  debugLog("Mega 2560 reboot");
  // Clock setup
  if (!rtc.begin()) {
    debugLog("RTC not found!");
    while (1);
  }
  if (!rtc.isrunning()) {
    debugLog("RTC not running, setting time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  DateTime dt = rtc.now();
  last_second = dt.second();
  measurements.setLastMeasurement(dt);

  // Relay and buttons pins setup
  relay_timer.setup(dt);
  pinMode(BUTTON_1, INPUT_PULLUP);

  // LCD setup
  lcd.init();
  lcd.backlight();

  // Sensors setup
  tempSensor.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // Atlas Scientific probes setup
  Serial2.begin(9600);
  Serial3.begin(9600);

  // Setting water level after reset
  measurements.recoverWaterLevel();
  measurements.update(dt);

  Serial2.print(F("R\r"));
  Serial3.print(F("R\r"));

  // Letting everything boot
  delay(1000);
  wdt_enable(WDTO_4S);
}

void loop() {
  wdt_reset();

  DateTime dt = rtc.now();
  measurements.serialCheck();

  if(dt.second() != last_second){
    measurements.update(dt);
    relay_timer.update(dt);

    if (digitalRead(BUTTON_1) == LOW)
      relay_timer.printStatus();
    else
      measurements.printStatus();

    last_second = dt.second();
  }

  checkESP32Messages();
}