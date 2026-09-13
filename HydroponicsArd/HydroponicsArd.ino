#include <RTClib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h>
#include <EEPROM.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

RTC_DS1307 rtc;

// EEPROM address to store timer config
#define EEPROM_ADDR 0
#define EEPROM_MAGIC 0xAB  // sentinel to detect valid data

// Button and pump leg shift parameters
#define RELAY_1 22
#define RELAY_2 23
#define BUTTON_1 24

bool buttonOneStatus;

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
unsigned long previousMillis;
const long interval = 1000;

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

// Valid timestamp check
bool isValidTimestamp(DateTime& dt) {
    return dt.year()   >= 2020 && dt.year()   <= 2100 &&
           dt.month()  >= 1    && dt.month()  <= 12   &&
           dt.day()    >= 1    && dt.day()    <= 31   &&
           dt.hour()   <= 23   &&
           dt.minute() <= 59   &&
           dt.second() <= 59;
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

// Generating measurement call for Atlas Scientific sensors
void ASMeasurementString(char *str, size_t len, float temp){
  if(temp < -99.9f) temp = -99.9f;
  if(temp >  999.9f) temp = 999.9f;
  char buffer1[8];
  dtostrf(temp, 0, 1, buffer1);
  snprintf(str, len, "RT,%s\r", buffer1);
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

// Controller config class
struct Config{
  uint8_t magic;
  long pumpOn;
  long pumpCycle;
  long lightOn;
  long lightOff;
  long readingInterval;

private:
  void save() const{
    EEPROM.put(EEPROM_ADDR, *this);
  }

public:
  // Clecking if config is in EEPROM and loading it
  void load(){
    EEPROM.get(EEPROM_ADDR, *this);

    char localBuf[90];
    if(magic == EEPROM_MAGIC){
      char lightOnBuf[6], lightOffBuf[6];
      formatCountdown(lightOn, lightOnBuf, sizeof(lightOnBuf));
      formatCountdown(lightOff, lightOffBuf, sizeof(lightOffBuf));

      snprintf(localBuf, sizeof(localBuf), "Loaded from EEPROM: Pump ON=%ld OFF=%ld | Light ON=%s OFF=%s | ReadingInt=%ld",
               pumpOn/60, (pumpCycle - pumpOn)/60, lightOnBuf, lightOffBuf, readingInterval);
      debugLog(localBuf);
    }
    else{
      debugLog("No valid EEPROM data, using defaults");
      save();
    }
  }

  void updateConfig(long _pumpOn, long _pumpCycle, long _lightOn, long _lightOff, long _readingInterval){
    pumpOn = _pumpOn; pumpCycle = _pumpCycle; lightOn = _lightOn; lightOff = _lightOff;
    readingInterval = _readingInterval;
    save();
    sendReport();

    char lightOnBuf[6], lightOffBuf[6];
    formatCountdown(lightOn, lightOnBuf, sizeof(lightOnBuf));
    formatCountdown(lightOff, lightOffBuf, sizeof(lightOffBuf));

    char localBuf[90];
    snprintf(localBuf, sizeof(localBuf), "Config updated: Pump ON=%ld OFF=%ld | Light ON=%s OFF=%s | ReadingInt=%ld",
             pumpOn/60, (pumpCycle - pumpOn)/60, lightOnBuf, lightOffBuf, readingInterval);
    debugLog(localBuf);
  }

  void sendReport() const{
    doc.clear();
    doc["cmd"]       = "configReport";
    doc["pumpOn"]    = pumpOn;
    doc["pumpCycle"] = pumpCycle;
    doc["lightOn"]   = lightOn;
    doc["lightOff"]  = lightOff;
    doc["readingInt"]  = readingInterval;

    serializeJson(doc, serialESPBuffer, sizeof(serialESPBuffer));
    Serial1.println(serialESPBuffer);

    Serial.print("Sent configReport to ESP32: ");
    Serial.println(serialESPBuffer);
  }
} config({EEPROM_MAGIC, 900, 3600, timeFromHMS(6,0,0), timeFromHMS(22,0,0), 60});

static_assert(__is_trivially_copyable(Config), "Config must be trivially copyable for EEPROM.put");

class LCDControl{
  LiquidCrystal_I2C lcd;
  char lcdLine0[17];
  char lcdLine1[17];

public:
  LCDControl(): lcd(0x27, 16, 2) {
    memset(lcdLine0, ' ', 16);
    lcdLine0[16] = '\0';
    memset(lcdLine1, ' ', 16);
    lcdLine1[16] = '\0';
  };

  void setup(){
    lcd.init();
    lcd.backlight();
  }

  void reset(){
    setup();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(lcdLine0);
    lcd.setCursor(0, 1);
    lcd.print(lcdLine1);
  }

  void print(const char *newLine0, size_t len0, const char *newLine1, size_t len1){
    bool changed = false;
    if(strcmp(lcdLine0, newLine0) != 0){
      memset(lcdLine0, ' ', 16);
      lcdLine0[16] = '\0';
      strncpy(lcdLine0, newLine0, len0 < 16 ? len0 : 16);
      changed = true;
    }
    if(strcmp(lcdLine1, newLine1) != 0){
      memset(lcdLine1, ' ', 16);
      lcdLine1[16] = '\0';
      strncpy(lcdLine1, newLine1, len1 < 16 ? len1 : 16);
      changed = true;
    }

    if(changed){
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(lcdLine0);
      lcd.setCursor(0, 1);
      lcd.print(lcdLine1);
    }
  }
} lcdControl;

// Timer code
class RelayTimer{
  const Config &cfg;

  bool pumpIsOn;
  bool lightIsOn;
  uint32_t totalSeconds;
  long daySeconds;
  long cycleSeconds;
  uint32_t pumpOffTime;

  bool pumpOverride;
  bool lightOverride;

  void switchPumpOn(){
    digitalWrite(RELAY_1, LOW);
    if(!pumpIsOn){
      lcdControl.reset();
      pumpIsOn = true;
      debugLog("Pump switched on");
    }
  }

  void switchPumpOff(){
    digitalWrite(RELAY_1, HIGH);
    if(pumpIsOn){
      lcdControl.reset();
      pumpIsOn = false;
      pumpOffTime = totalSeconds;
      debugLog("Pump switched off");
    }
  }

  void switchLightOn(){
    digitalWrite(RELAY_2, LOW);
    if(!lightIsOn){
      lcdControl.reset();
      lightIsOn = true;
      debugLog("Light switched on");
    }
  }

  void switchLightOff(){
    digitalWrite(RELAY_2, HIGH);
    if(lightIsOn){
      lcdControl.reset();
      lightIsOn = false;
      debugLog("Light switched off");
    }
  }

public:
  RelayTimer(const Config &_cfg): cfg(_cfg),
    pumpIsOn(false), lightIsOn(false), totalSeconds(0), daySeconds(0), cycleSeconds(0),
    pumpOffTime(0), pumpOverride(false), lightOverride(false) {};

  bool isPumpOn() const {return pumpIsOn;}
  bool isLightOn() const {return lightIsOn;}
  long timeAfterPumpOff() const {
    if(pumpIsOn)
      return 0;
    else
      return totalSeconds - pumpOffTime;
  }

  void setup(const DateTime &dt){
    // Setting up initial relay configuration
    totalSeconds = dt.unixtime();
    daySeconds = totalSeconds % 86400;
    cycleSeconds = totalSeconds % cfg.pumpCycle;

    pinMode(RELAY_1, OUTPUT);
    if(cycleSeconds < cfg.pumpOn){
      digitalWrite(RELAY_1, LOW);
      pumpIsOn = true;
    }
    else{
      digitalWrite(RELAY_1, HIGH);
      pumpIsOn = false;
      pumpOffTime = (totalSeconds/cfg.pumpCycle)*cfg.pumpCycle + cfg.pumpOn;
    }

    pinMode(RELAY_2, OUTPUT);
    if(daySeconds >= cfg.lightOn && daySeconds < cfg.lightOff){
      digitalWrite(RELAY_2, LOW);
      lightIsOn = true;
    }
    else{
      digitalWrite(RELAY_2, HIGH);
      lightIsOn = false;
    }

    char localBuf[80];
    snprintf(localBuf, sizeof(localBuf), "At reboot Pump %s Light %s", pumpIsOn ? "ON" : "OFF", lightIsOn ? "ON" : "OFF");
    debugLog(localBuf);
  }

  void update(const DateTime &dt){
    totalSeconds = dt.unixtime();
    daySeconds = totalSeconds % 86400;
    cycleSeconds = totalSeconds % cfg.pumpCycle;

    if(!pumpOverride){
      if(cycleSeconds < cfg.pumpOn)
        switchPumpOn();
      else
        switchPumpOff();
    }

    if(!lightOverride){
      if(daySeconds >= cfg.lightOn && daySeconds < cfg.lightOff)
        switchLightOn();
      else
        switchLightOff();
    }
  }

  void printStatus() const{
    char cntBuf[6];

    if(!pumpOverride){
      memset(line0, ' ', 16);
      line0[16] = '\0';
      if (cycleSeconds < cfg.pumpOn) {
        memcpy(line0, "Pump off: ", 10);
        formatCountdown(cfg.pumpOn - cycleSeconds, cntBuf, sizeof(cntBuf));
        memcpy(line0 + 10, cntBuf, 5);
      } else {
        memcpy(line0, "Pump on:  ", 10);
        formatCountdown(cfg.pumpCycle - cycleSeconds, cntBuf, sizeof(cntBuf));
        memcpy(line0 + 10, cntBuf, 5);
      }
    }
    else{
      snprintf(line0, sizeof(line0), "Pump override");
    }

    if(!lightOverride){
      memset(line1, ' ', 16);
      line1[16] = '\0';
      if(daySeconds >= cfg.lightOn && daySeconds < cfg.lightOff){
        memcpy(line1, "Light off: ", 11);
        formatCountdown(cfg.lightOff - daySeconds, cntBuf, sizeof(cntBuf));
        memcpy(line1 + 11, cntBuf, 5);
      }
      else{
        memcpy(line1, "Light on:  ", 11);
        long t = cfg.lightOn < daySeconds ? 86400 + cfg.lightOn - daySeconds : cfg.lightOn - daySeconds;
        formatCountdown(t, cntBuf, sizeof(cntBuf));
        memcpy(line1 + 11, cntBuf, 5);
      }
    }
    else{
      snprintf(line1, sizeof(line1), "Light override");
    }

    lcdControl.print(line0, sizeof(line0), line1, sizeof(line1));
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

} relay_timer(config);

// Measurement code
class Measurements{
  const Config &cfg;
  long levelMeasureCooldown;
  uint32_t lastSave;

  float waterTemp;
  float waterLevel;
  float ph;
  float ec;

  bool readPH;
  bool readEC;
  bool waterLevelFrozen;

  void sendReading() const{
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
  Measurements(const Config &_cfg, long _levelMeasureCooldown): cfg(_cfg),
      levelMeasureCooldown(_levelMeasureCooldown), lastSave(0), waterTemp(0), waterLevel(-1.0f), ph(7), ec(1),
      readPH(false), readEC(false), waterLevelFrozen(false) {}

  void setLastMeasurement(const DateTime &dt){
    lastSave = dt.unixtime();
  }

  float getWaterTemp() const {return waterTemp;}

  void recoverWaterLevel(){
    if(ds1307_read_byte(SRAM_FLAG_ADDR) == 1){
      waterLevel = sram_read_float(SRAM_FLOAT_ADDR);
      waterLevelFrozen = true;
    }
    else
      waterLevelFrozen = false;
  }

  void update(const DateTime &dt){
    waterTemp = tempSensor.getTempCByIndex(0);
    tempSensor.requestTemperatures();
    if(relay_timer.timeAfterPumpOff() >= levelMeasureCooldown){
      // waterLevel = 24.6 - readLevel(); // Bucket depth with no water as measured by ultrasound is 24.6
      waterLevel = 0;
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

    uint32_t now = dt.unixtime();
    if(now < lastSave) lastSave = now;  // guard against RTC glitch
    long saveTimer = (long)(now - lastSave) - cfg.readingInterval;
    if(saveTimer >= -4 && !readPH){
      char buffer1[16];
      ASMeasurementString(buffer1, sizeof(buffer1), waterTemp);
      Serial2.print(buffer1);
      readPH = true;
    }
    if(saveTimer >= -2 && !readEC){
      char buffer1[16];
      ASMeasurementString(buffer1, sizeof(buffer1), waterTemp);
      Serial3.print(buffer1);
      readEC = true;
    }
    if(saveTimer >= 0){
      sendReading();
      lastSave = now;
      readPH = false;
      readEC = false;
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

    if(Serial3.available()){
      int len = Serial3.readBytesUntil('\r', serialBuffer, sizeof(serialBuffer) - 1);
      serialBuffer[len] = '\0';
      if(isValidFloat(serialBuffer)){
        float val = atof(serialBuffer);
        if(val >= 0.0) ec = val/1000;
      }
    }
  }

  void printStatus() const{
    char buffer1[6], buffer2[6];

    memset(line0, ' ', 16);
    line0[16] = '\0'; 
    dtostrf(ph, 5, 2, buffer1);
    dtostrf(ec, 5, 2, buffer2);
    snprintf(line0, sizeof(line0), "pH%s EC%s", buffer1, buffer2);

    memset(line1, ' ', 16);
    line1[16] = '\0'; 
    dtostrf(waterTemp, 5, 1, buffer1);
    // dtostrf(waterLevel, 6, 1, buffer2);
    // snprintf(line1, sizeof(line1), "T%s L%s", buffer1, buffer2);
    snprintf(line1, sizeof(line1), "Temp %s", buffer1);

    lcdControl.print(line0, sizeof(line0), line1, sizeof(line1));
  }
} measurements(config, 30);

// ESP32 receiver function
void checkESP32Messages() {
  while (Serial1.available()) {
    int len = Serial1.readBytesUntil('\n', serialESPBuffer, sizeof(serialESPBuffer)-1);
    if (len > 0 && serialESPBuffer[len-1] == '\r') len--;  // trim trailing \r
    serialESPBuffer[len] = '\0';

    if(len > 0){
      Serial.print(F("ESP32: "));
      Serial.println(serialESPBuffer);
    }

    DeserializationError err = deserializeJson(doc, serialESPBuffer);
    if(!err){
      if(doc.containsKey("cmd")){
        const char* cmd = doc["cmd"];
        if(strcmp(cmd, "setConfig") == 0){
          if(doc.containsKey("pumpOn") && doc.containsKey("pumpCycle") && doc.containsKey("lightOn") && doc.containsKey("lightOff") && doc.containsKey("readingInt")){
            config.updateConfig(doc["pumpOn"].as<long>(), doc["pumpCycle"].as<long>(), doc["lightOn"].as<long>(), doc["lightOff"].as<long>(), doc["readingInt"].as<long>());
          }
          else{
            debugLog("setConfig missing fields");
          }
        }
        else if(strcmp(cmd, "getConfig") == 0){
          config.sendReport();
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
        else if(strcmp(cmd, "setTime") == 0){
          if(doc.containsKey("y") && doc.containsKey("mo") && doc.containsKey("d") &&
             doc.containsKey("h") && doc.containsKey("mi") && doc.containsKey("s"))
          {
            DateTime newTime(doc["y"].as<uint16_t>(), doc["mo"].as<uint8_t>(), doc["d"].as<uint8_t>(),
                            doc["h"].as<uint8_t>(), doc["mi"].as<uint8_t>(), doc["s"].as<uint8_t>());
            if(isValidTimestamp(newTime)){
              long drift = (long) newTime.unixtime() - (long) rtc.now().unixtime();
              if(drift >= 2 || drift <= -2){
                rtc.adjust(newTime);
                measurements.setLastMeasurement(newTime);   // keep the reading timer sane
                char localBuf[64];
                snprintf(localBuf, sizeof(localBuf), "RTC synced, drift was %ld s", drift);
                debugLog(localBuf);
              }
            }
            else{
              debugLog("setTime out of range");
            }
          }
          else{
            debugLog("setTime missing fields");
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
  }
}

void setup() {
  MCUSR = 0;
  wdt_disable();

  // Debug serial
  Serial.begin(115200);
  // ESP32 serial
  Serial1.begin(115200);

  //Temperature sensor setup
  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();

  // Atlas Scientific probes setup
  Serial2.begin(9600);
  Serial3.begin(9600);

  delay(1000); // For serials to initialize and tempSensor to get first reading

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

  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  DateTime dt = rtc.now();
  char ctime_buff[40];
  snprintf(ctime_buff, sizeof(ctime_buff),"Current time %04d/%02d/%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  Serial.println(ctime_buff);

  config.load();

  previousMillis = 0;
  measurements.setLastMeasurement(dt);

  // Relay and buttons pins setup
  relay_timer.setup(dt);
  pinMode(BUTTON_1, INPUT_PULLUP);
  buttonOneStatus = (digitalRead(BUTTON_1) == LOW);

  // LCD setup
  lcdControl.setup();

  // Water level sensor setup
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // Setting water level and reading temperature after reset
  measurements.recoverWaterLevel();
  measurements.update(dt);

  // Reading pH and EC after reset
  char buffer1[16];
  ASMeasurementString(buffer1, sizeof(buffer1), measurements.getWaterTemp());
  Serial2.print(buffer1);
  Serial3.print(buffer1);

  // Letting everything boot
  delay(1000);
  wdt_enable(WDTO_4S);
}

void loop() {
  wdt_reset();
  measurements.serialCheck();

  unsigned long currentMillis = millis();
  if(currentMillis - previousMillis >= interval){
    DateTime dt = rtc.now();
    if(!isValidTimestamp(dt)) return;
    measurements.update(dt);
    relay_timer.update(dt);

    if(digitalRead(BUTTON_1) == LOW){
      if(!buttonOneStatus)
        buttonOneStatus = true;
      relay_timer.printStatus();
    }
    else{
      if(buttonOneStatus)
        buttonOneStatus = false;
      measurements.printStatus();
    }

    previousMillis = currentMillis;
  }

  checkESP32Messages();
}