#include <RTClib.h>
#include <LiquidCrystal.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <dht_nonblocking.h>

RTC_DS1307 rtc;
LiquidCrystal lcd(23, 22, 27, 26, 25, 24);

#define BUTTON_1 28
#define LEG_SHIFT 30

#define ONE_WIRE_BUS 4
// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature tempSensor(&oneWire);

// Echo distance parameters
#define TRIG_PIN 5
#define ECHO_PIN 6
#define TIMEOUT_US 30000

// DHT crude temperature humidity sensor
#define DHT_SENSOR_TYPE DHT_TYPE_11
#define DHT_SENSOR_PIN 7
DHT_nonblocking dht_sensor(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);

// Temp variables
int last_second;
char line0[17];
char line1[17];

void debugLog(String msg) {
  Serial.println(msg);   // USB → PC Serial Monitor
}

void checkESP32Messages() {
  // Read any debug messages coming back from ESP32
  while (Serial1.available()) {
    String msg = Serial1.readStringUntil('\n');
    msg.trim();
    if (msg.length() > 0) {
      debugLog("ESP32: " + msg);
    }
  }
}

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

public:
  RelayTimer(long _pumpOn, long _pumpOff, long _lightOn, long _ligtsOff):
    pumpOn(_pumpOn), pumpCycle(_pumpOn+_pumpOff), lightOn(_lightOn), lightOff(_ligtsOff),
    pumpIsOn(false), lightIsOn(false), totalSeconds(0), cycleSeconds(0) {};

  bool isPumpOn() const {return pumpIsOn;}
  bool isLightOn() const {return lightIsOn;}
  unsigned long timeAfterPumpOff() const {
    if(pumpIsOn)
      return 0;
    else
      return cycleSeconds - pumpOn;
  }

  void update(const DateTime &dt){
    totalSeconds = timeFromHMS(dt.hour(), dt.minute(), dt.second());
    cycleSeconds = totalSeconds % pumpCycle;

    if(cycleSeconds < pumpOn){
      digitalWrite(LEG_SHIFT, LOW);
      if(!pumpIsOn){ // Resetting the screen after relay click
        delay(100);
        lcd.begin(16, 2);
        pumpIsOn = true;
      }
    }
    else{
      digitalWrite(LEG_SHIFT, HIGH);
      if(pumpIsOn){ // Resetting the screen after relay click
        delay(100);
        lcd.begin(16, 2);
        pumpIsOn = false;
      }
    }

    if(totalSeconds >= lightOn && totalSeconds < lightOff){
      digitalWrite(LEG_SHIFT+1, LOW);
      if(!lightIsOn){ // Resetting the screen after relay click
        delay(100);
        lcd.begin(16, 2);
        lightIsOn = true;
      }
    }
    else{
      digitalWrite(LEG_SHIFT+1, HIGH);
      if(lightIsOn) lcd.begin(16, 2);{ // Resetting the screen after relay click
        delay(100);
        lcd.begin(16, 2);
        lightIsOn = false;
      }
    }
  }

  void printStatus(){
    memset(line0, ' ', 16);
    line0[16] = '\0'; 
    if(cycleSeconds < pumpOn){
      snprintf(line0, sizeof(line0), "Pump off: ");
      formatCountdown(pumpOn-cycleSeconds, line0+10, 7);
    }
    else{
      snprintf(line0, sizeof(line0), "Pump on: ");
      formatCountdown(pumpCycle-cycleSeconds, line0+9, 8);
    }

    memset(line1, ' ', 16);
    line1[16] = '\0'; 
    if(totalSeconds >= lightOn && totalSeconds < lightOff){
      snprintf(line1, sizeof(line1), "Light off: ");
      formatCountdown(lightOff-totalSeconds, line1+11, 6);
    }
    else{
      snprintf(line1, sizeof(line1), "Light on: ");
      formatCountdown(lightOn < totalSeconds ? 86400+lightOn-totalSeconds: lightOn-totalSeconds, line1+10, 7);
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcd.setCursor(0, 1);
    lcd.print(line1);
  }

} relay_timer(900, 900, timeFromHMS(6,0,0), timeFromHMS(22,0,0));

// Measurement code
class Measurements{
  long databaseSaveInverval;
  long levelMeasureCooldown;
  uint32_t lastSave;

  float waterTemp;
  float waterLevel;
  float ph;
  float ec;
  float extTemp;
  float extHumidity;

public:
  Measurements(long _databaseSaveInverval, long _levelMeasureCooldown): databaseSaveInverval(_databaseSaveInverval),
    levelMeasureCooldown(_levelMeasureCooldown), lastSave(0), waterTemp(0), waterLevel(0), ph(7), ec(1), extTemp(0), extHumidity(0) {}

  void setLastMeasurement(const DateTime &dt){
    lastSave = dt.unixtime();
  }

  void update(const DateTime &dt){
    tempSensor.requestTemperatures();
    waterTemp = tempSensor.getTempCByIndex(0);
    if(relay_timer.timeAfterPumpOff() >= levelMeasureCooldown)
      waterLevel = 24.6 - readLevel(); // Bucket depth with no water as measured by ultrasound is 24.6
    dht_sensor.measure(&extTemp, &extHumidity);

    uint32_t timestamp = dt.unixtime();
    if(timestamp - lastSave >= databaseSaveInverval){
      StaticJsonDocument<256> doc;
      doc["temp"] = waterTemp;
      doc["ph"] = ph;
      doc["ec"] = ec;
      doc["wlevel"] = waterLevel;
      doc["etemp"] = extTemp;
      doc["erh"] = extHumidity;
      doc["pump"] = relay_timer.isPumpOn();
      doc["light"] = relay_timer.isLightOn();

      String payload;
      serializeJson(doc, payload);

      Serial1.println(payload);   // → ESP32 via TX1/RX1 (pins 18/19)
      debugLog("Sent: " + payload);
      lastSave = timestamp;
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

void setup() {
  // Clock setup
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    while (1);
  }
  if (!rtc.isrunning()) {
    Serial.println("RTC not running, setting time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  DateTime dt = rtc.now();
  last_second = dt.second();
  measurements.setLastMeasurement(dt);

  // LCD setup
  lcd.begin(16, 2); // Number of rows and columns

  // Relay and buttons pins setup
  for(int i = LEG_SHIFT; i < LEG_SHIFT+4; ++i){
    pinMode(i, OUTPUT);
    digitalWrite(i, HIGH);
  }

  pinMode(BUTTON_1, INPUT_PULLUP);

  // Sensors setup
  tempSensor.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Serial.begin(115200);
  Serial1.begin(115200);

  // Getting a reading before pump is switched on
  measurements.update(dt);
}

void loop() {
  DateTime dt = rtc.now();
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