#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h> //https://arduinojson.org/v6/doc/
#include <time.h>

String WIFI_SSID = "xxx";
String WIFI_PASS = "xxx";
String WIFI_HOSTNAME = "FeatherWeather";
String MY_KEY = "xxx"; // Used to connect to an script running on pythonanywhere.com which parses the weather data
                      // It parses there because the memory on this board cannot handle the size of the API data

struct weather
{
   long theTime;
   const char* summary;
   const char* icon;
   float temperature;
   float apparentTemperature;
   float temperatureHigh;
   float temperatureLow;
};

struct weather currentWeather;
struct weather hourlyWeather[6];
struct weather dailyWeather[6];



/*******************************************************
 * SCREEN SETUP AND RELATED
 * *****************************************************/
#include <SPI.h>
#include <Adafruit_STMPE610.h>
#include "TouchControllerWS.h"
#include <MiniGrafx.h>
#include <ILI9341_SPI.h>

/* Feather Huzzah + 2.4" TFT wing */
// Pins for the ILI9341
#define TFT_DC 15
#define TFT_CS 0
#define TFT_LED 5
#define TOUCH_CS 16

#define MINI_BLACK 0
#define MINI_WHITE 1
#define MINI_YELLOW 2
#define MINI_BLUE 3

// defines the colors usable in the paletted 16 color frame buffer
uint16_t palette[] = {ILI9341_BLACK, // 0
                      ILI9341_WHITE, // 1
                      ILI9341_YELLOW, // 2
                      0x7E3C
                      }; //3
                      
int SCREEN_WIDTH = 240;
int SCREEN_HEIGHT = 320;
// Limited to 4 colors due to memory constraints
int BITS_PER_PIXEL = 2; // 2^2 =  4 colors
ADC_MODE(ADC_VCC);

ILI9341_SPI tft = ILI9341_SPI(TFT_CS, TFT_DC);
MiniGrafx gfx = MiniGrafx(&tft, BITS_PER_PIXEL, palette);
Adafruit_STMPE610 ts(TOUCH_CS);
TouchControllerWS touchController(&ts);

void calibrationCallback(int16_t x, int16_t y) {
  gfx.setColor(1);
  gfx.fillCircle(x, y, 10);
}

CalibrationCallback calibration = &calibrationCallback;
int screenCount = 3; // how many different screens do we have?
uint16_t screen = 0;

void setupLED(){
  // Turn on the background LED
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);    // HIGH to Turn on;
  delay(100); 
  gfx.init();
  gfx.fillBuffer(MINI_BLACK);
  gfx.commit();  
}

void initializeTouchScreen(){
  Serial.println("Initializing touch screen...");
  if (!ts.begin()) {
    Serial.println("Couldn't start touchscreen controller");
    while (1);
  }
}

void setupFileSystem(){
  Serial.println("Mounting file system...");
  bool isFSMounted = SPIFFS.begin();
  if (!isFSMounted) {
    Serial.println("Formatting file system...");
    SPIFFS.format();
  }
}

void calibrateTouchscreen(){
  SPIFFS.remove("/calibration.txt");
  boolean isCalibrationAvailable = touchController.loadCalibration();
  if (!isCalibrationAvailable) {
    Serial.println("Calibration not available");
    touchController.startCalibration(&calibration);
    while (!touchController.isCalibrationFinished()) {
      gfx.fillBuffer(0);
      gfx.setColor(MINI_YELLOW);
      gfx.setTextAlignment(TEXT_ALIGN_CENTER);
      gfx.drawString(120, 160, "Please calibrate\ntouch screen by\ntouch point");
      touchController.continueCalibration();
      gfx.commit();
      yield();
    }
    touchController.saveCalibration();
  } else {
    Serial.println("Touchscreen calibrated");
  }
}



/*******************************************************
 * WIFI SETUP AND RELATED
 * *****************************************************/

//Connect to WIFI
void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("");
  Serial.print("Connecting to WiFi");
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.hostname(WIFI_HOSTNAME);
  WiFi.begin(WIFI_SSID,WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.print("\nConnected to " + String(WIFI_SSID) + ", IP: ");
  Serial.println(WiFi.localIP());
}




/*******************************************************
 * CLOCK/TIME SETUP AND RELATED
 * *****************************************************/
#include <simpleDSTadjust.h>
const int UPDATE_WEATHER_INTERVAL_SECS = 10 * 60; // Update every 10 minutes
const int UPDATE_CLOCK_INTERVAL_SECS = 12 * 60 * 60; // Update every 12 hours
#define UTC_OFFSET -7 // Settings for Denver
//TimeChangeRule myRule = {abbrev, week, dow, month, hour, offset};
struct dstRule StartRule = {"MDT", Second, Sun, Mar, 2, 3600}; // Mountain Daylight time = UTC/GMT -6 hours
struct dstRule EndRule = {"MST", First, Sun, Nov, 2, 0};       // Mountain Standard time = UTC/GMT -7 hour
 #define NTP_SERVERS "us.pool.ntp.org", "time.nist.gov", "pool.ntp.org"

simpleDSTadjust dstAdjusted(StartRule, EndRule);
long lastWeatherUpdate = millis();
long lastClockUpdate = millis();
time_t dstOffset = 0;
char LAST_UPDATED[12]; // Last updated time

void updateTime(){
  Serial.println("Updating time...");
  configTime(UTC_OFFSET * 3600, 0, NTP_SERVERS);
  //while(!time(nullptr)) {
  while(time(nullptr) <= 100000) {
    Serial.print(".");
    delay(500);
  }
  dstOffset = UTC_OFFSET * 3600 + dstAdjusted.time(nullptr) - time(nullptr);
}

//Data last updated time - uses global LAST_UPDATED 
void setLastUpdatedTime(){
  char *dstAbbrev;
  time_t now = dstAdjusted.time(&dstAbbrev);
  struct tm * timeinfo = localtime (&now);
  int hour = (timeinfo->tm_hour+11)%12+1;  // take care of noon and midnight
  sprintf(LAST_UPDATED, "%2d:%02d %s",hour, timeinfo->tm_min, timeinfo->tm_hour>=12?"PM":"AM");
}





/*******************************************************
 * HTTP REQUEST AND RELATED
 * *****************************************************/
void updateData(){
  HTTPClient http;
  Serial.println("Requesting data...");
  http.begin("http://...pythonanywhereURLhere...");
  http.addHeader("Content-Type", "text/plain");
  int httpCode = http.POST("{\"key\":\"" + MY_KEY + "\"}");
  String payload = http.getString();
  Serial.print("HTTP Response: ");
  Serial.println(httpCode);
  Serial.println(payload);
  parseWeather(payload);
  http.end();
  setLastUpdatedTime();
}

void parseWeather(String payload){
  //https://arduinojson.org/v6/assistant/
  const size_t capacity = 2*JSON_ARRAY_SIZE(6) + 2*JSON_OBJECT_SIZE(2) + 7*JSON_OBJECT_SIZE(3) + 6*JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(7) + 840;

  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, payload);

  // Get all current data
  JsonObject current = doc["current"];
  currentWeather.theTime = current["time"]; // 1583100000
  currentWeather.summary = current["summary"]; // "Mostly Cloudy"
  currentWeather.icon =  current["icon"]; // "partly-cloudy-day"
  currentWeather.temperature = current["temperature"]; // 40.72
  currentWeather.apparentTemperature = current["apparentTemperature"]; // 35.49
  currentWeather.temperatureHigh = current["temperatureHigh"]; // 42.39
  currentWeather.temperatureLow = current["temperatureLow"]; // 24.07

  // Get all hourly data
  hourlyWeather[0].summary = doc["hourly"]["hourlySummary"]; // "Foggy this evening and tonight."
  JsonArray hourly_data = doc["hourly"]["data"];
  for (int i = 0; i < 6; i++){
    JsonObject hourly_data_i = hourly_data[i];
    hourlyWeather[i].theTime = hourly_data_i["time"]; // 1583100000
    hourlyWeather[i].icon = hourly_data_i["icon"]; // "partly-cloudy-day"
    hourlyWeather[i].temperature = hourly_data_i["temperature"]; // 41.63
  }
 
  // Get all daily data
  dailyWeather[0].summary = doc["daily"]["weeklySummary"]; // "Possible light snow today and tomorrow."
  JsonArray daily_data = doc["daily"]["data"];
  for (int i = 0; i < 6; i++){
    JsonObject daily_data_i = daily_data[i];
    dailyWeather[i].theTime = daily_data_i["time"]; // 1583132400
    dailyWeather[i].icon = daily_data_i["icon"]; // "snow"
    dailyWeather[i].temperatureHigh = daily_data_i["temperatureHigh"]; // 42.8
    dailyWeather[i].temperatureLow = daily_data_i["temperatureLow"]; // 23.58
  } 
}



/*******************************************************
 * SCREEN 'DRAWINGS'
 * *****************************************************/
#include "ArialRounded.h"
#include "weathericons.h"

void drawTime() {
  char time_str[11];
  char *dstAbbrev;
  time_t now = dstAdjusted.time(&dstAbbrev);
  struct tm * timeinfo = localtime (&now);
  
  // Date
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_WHITE);
  String date = ctime(&now);
  date = date.substring(0,11) + String(", ") + String(1900 + timeinfo->tm_year);
  gfx.drawString(SCREEN_WIDTH/2, 2, date);
  
  // Time
  gfx.setFont(ArialRoundedMTBold_36);
  int hour = (timeinfo->tm_hour+11)%12+1;  // take care of noon and midnight
  sprintf(time_str, "%02d:%02d:%02d\n",hour, timeinfo->tm_min, timeinfo->tm_sec);
  gfx.drawString(SCREEN_WIDTH/2, 16, time_str);

  // MST, AM/PM
  gfx.setTextAlignment(TEXT_ALIGN_LEFT);
  gfx.setFont(ArialMT_Plain_10);
  gfx.setColor(MINI_BLUE);
  sprintf(time_str, "%s\n%s", dstAbbrev, timeinfo->tm_hour>=12?"PM":"AM");
  gfx.drawString(200, 27, time_str);
}

// converts the dBm to a range between 0 and 100%
int8_t getWifiQuality() {
  int32_t dbm = WiFi.RSSI();
  if(dbm <= -100) {
      return 0;
  } else if(dbm >= -50) {
      return 100;
  } else {
      return 2 * (dbm + 100);
  }
}

void drawWifiQuality() {
  int8_t quality = getWifiQuality();
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);  
  gfx.drawString(228, 5, String(quality) + "%");
  for (int8_t i = 0; i < 4; i++) {
    for (int8_t j = 0; j < 2 * (i + 1); j++) {
      if (quality > i * 25 || j == 0) {
        gfx.setPixel(230 + 2 * i, 14 - j);
      }
    }
  }
}

void drawCurrentWeather() {
  // Icon
  gfx.setTransparentColor(MINI_BLACK);
  gfx.drawPalettedBitmapFromPgm(28, 60, getMeteoconIconFromProgmem(currentWeather.icon));
  //gfx.drawPalettedBitmapFromPgm(28, 60, getMeteoconIconFromProgmem("clear-night"));
  
  // City, State
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_BLUE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.drawString(220, 60, "Centennial, CO");

  // Current temperature
  gfx.setFont(ArialRoundedMTBold_36);
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.drawString(220, 73, String(currentWeather.temperature, 0) + "°F");

  // Real feel
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.drawString(220, 111, "Feels Like: " + String(currentWeather.apparentTemperature, 0) + "°F");
  
  // High/Low
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  String highLow = "L: " + String(currentWeather.temperatureLow, 0) + "°F  H:" + String(currentWeather.temperatureHigh, 0) + "°F";
  gfx.drawString(220, 127, highLow);

  // Current Summary
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.drawString(60, 127, currentWeather.summary);
}

void drawHourlyWeather(){
  // Hourly Summary
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_BLUE);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  // Felt very cramped with the below, so I have removed
  /*if (String(hourlyWeather[0].summary).length() > 30){
    gfx.drawStringMaxWidth(SCREEN_WIDTH/2, 150, 220, hourlyWeather[0].summary);
  } else {
    gfx.drawStringMaxWidth(SCREEN_WIDTH/2, 158, 220, hourlyWeather[0].summary);
  }*/

  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  int currentY = 153;
  int startX = 40;
  
  // Loop through 6 hourly forecasts
  for (int j = 0; j < 2; j++){
    int currentX = startX;
    for (int i = 0; i <= 2; i++){
      gfx.setColor(MINI_BLUE);
      time_t time = hourlyWeather[i+(3*j)].theTime + dstOffset;
      gfx.drawString(currentX, currentY, getHour(&time));
      
      gfx.setColor(MINI_WHITE);
      gfx.drawString(currentX, currentY+15, String(hourlyWeather[i+(3*j)].temperature,0) + "°F");
      
      gfx.drawPalettedBitmapFromPgm(currentX-25, currentY+30, getMiniMeteoconIconFromProgmem(hourlyWeather[i+(3*j)].icon));
      currentX += 80;
    }
    currentY += 77;
  }
}


void drawDailyWeather(){
  // Daily Summary
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_BLUE);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  // Felt very cramped with the below, so I have removed
  /*if (String(dailyWeather[0].summary).length() > 30){
    gfx.drawStringMaxWidth(SCREEN_WIDTH/2, 150, 220, dailyWeather[0].summary);
  } else {
    gfx.drawStringMaxWidth(SCREEN_WIDTH/2, 158, 220, dailyWeather[0].summary);
  }*/

  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  int currentY = 153;
  int startX = 40;
  
  // Loop through 6 daily forecasts
  for (int j = 0; j < 2; j++){
    int currentX = startX;
    for (int i = 0; i <= 2; i++){
      gfx.setColor(MINI_BLUE);
      // Removed the + dstOffset here because the conversion was causing multiple 'days' to show as the same 'DOW'
      time_t time = dailyWeather[i+(3*j)].theTime;
      String date = ctime(&time);
      date = date.substring(0,4);
      gfx.drawString(currentX, currentY, date);
      
      gfx.setColor(MINI_WHITE);
      gfx.drawString(currentX, currentY+15, "L:" + String(dailyWeather[i+(3*j)].temperatureLow,0) + " H:" + String(dailyWeather[i+(3*j)].temperatureHigh,0));
      
      gfx.drawPalettedBitmapFromPgm(currentX-25, currentY+30, getMiniMeteoconIconFromProgmem(dailyWeather[i+(3*j)].icon));
      currentX += 80;
    }
    currentY += 77;
  }
}


void drawAbout() {
  gfx.setFont(ArialRoundedMTBold_14);
  drawLabelValue(0, "Heap Mem:", String(ESP.getFreeHeap() / 1024)+"kb");
  drawLabelValue(1, "Flash Mem:", String(ESP.getFlashChipRealSize() / 1024 / 1024) + "MB");
  drawLabelValue(2, "WiFi Strength:", String(WiFi.RSSI()) + "dB");
  drawLabelValue(3, "Chip ID:", String(ESP.getChipId()));
  drawLabelValue(4, "VCC:", String(ESP.getVcc() / 1024.0) +"V");
  drawLabelValue(5, "CPU Freq:", String(ESP.getCpuFreqMHz()) + "MHz");
  char time_str[15];
  const uint32_t millis_in_day = 1000 * 60 * 60 * 24;
  const uint32_t millis_in_hour = 1000 * 60 * 60;
  const uint32_t millis_in_minute = 1000 * 60;
  uint8_t days = millis() / (millis_in_day);
  uint8_t hours = (millis() - (days * millis_in_day)) / millis_in_hour;
  uint8_t minutes = (millis() - (days * millis_in_day) - (hours * millis_in_hour)) / millis_in_minute;
  sprintf(time_str, "%2dd%2dh%2dm", days, hours, minutes);
  drawLabelValue(6, "Uptime:", time_str);
  // Removed last item as the text doesn't fit on the screen with this setup
  //drawLabelValue(7, "Last Reset:", ESP.getResetInfo());
}


void drawLastUpdated(){
  // last updated time at bottom
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setFont(ArialMT_Plain_10);
  gfx.setColor(MINI_BLUE);
  gfx.drawString(SCREEN_WIDTH/2, 310, "Weather Last Updated: " + String(LAST_UPDATED));
}



/*******************************************************
 * BOARD INITIAL SETUP
 * *****************************************************/
void setup() {
  Serial.begin(115200);
  setupLED();
  gfx.setRotation(2);
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setColor(MINI_WHITE);

  gfx.fillBuffer(MINI_BLACK);
  gfx.drawString(SCREEN_WIDTH/2, SCREEN_HEIGHT/2, "Connecting to WIFI...");
  gfx.commit();
  connectWifi();

  gfx.fillBuffer(MINI_BLACK);
  gfx.drawString(SCREEN_WIDTH/2, SCREEN_HEIGHT/2, "Setting up file system & \ntouch screen...");
  gfx.commit();
  initializeTouchScreen();
  //setupFileSystem();
  //calibrateTouchscreen();
  
  if(WiFi.status() != WL_CONNECTED){
    Serial.println("Error in WiFi Connection, unable to update data");
  } else {
    
    gfx.fillBuffer(MINI_BLACK);
    gfx.drawString(SCREEN_WIDTH/2, SCREEN_HEIGHT/2, "Updating Clock...");
    gfx.commit();
    updateTime();
    
    gfx.fillBuffer(MINI_BLACK);
    gfx.drawString(SCREEN_WIDTH/2, SCREEN_HEIGHT/2, "Updating Weather...");
    gfx.commit();
    updateData();
  }
}


/*******************************************************
 * BOARD MAIN LOOP
 * *****************************************************/
void loop() {
  // Check if we should update weather information
  if (millis() - lastWeatherUpdate > 1000 * UPDATE_WEATHER_INTERVAL_SECS) {
      lastWeatherUpdate = millis();
      if(WiFi.status() != WL_CONNECTED){
        Serial.println("Error in WiFi Connection, unable to update data");
      } else {
        // Only update the clock every so often
        if (millis() - lastClockUpdate > 1000 * UPDATE_CLOCK_INTERVAL_SECS) {
          updateTime();
          lastClockUpdate = millis();
        }
        updateData();
      }
  }

  if (touchController.isTouched(0)) {
    TS_Point p = touchController.getPoint();
    Serial.print("Touchscreen touch: "); 
    Serial.print(p.x); Serial.print(", "); 
    Serial.print(p.y);Serial.print(", "); 
    Serial.println(p.z);
    screen = (screen + 1) % screenCount;
  }
  
  gfx.fillBuffer(MINI_BLACK);
  drawTime();           // Draw clock
  drawWifiQuality();    // Draw WIFI signal
  drawCurrentWeather(); // Current weather details
  drawLastUpdated();    // Last updated time

  if (screen == 0){
    drawDailyWeather();   // Daily outlook
  } else if (screen == 1){
    drawHourlyWeather();  // Hourly outlook
  } else {
    drawAbout();          // About info
  }

  gfx.commit();         // Load screen
  
  //Serial.print("After Update - Available Heap: ");
  //Serial.println(ESP.getFreeHeap());
  //delay(120000); // every 120 seconds
}


// Helper function to extract the hour out of a time stamp for the hourly weather outlook
String getHour(time_t *timestamp) {
  struct tm *timeInfo = gmtime(timestamp);
  char buf[6];
  int hour = (timeInfo->tm_hour+11)%12+1;  // take care of noon and midnight
  sprintf(buf, "%2d %s", hour, timeInfo->tm_hour>=12?"PM":"AM");
  return String(buf);
}


// Helper function used in the 'drawAbout' function
void drawLabelValue(uint8_t line, String label, String value) {
  const uint8_t labelX = 115;
  const uint8_t valueX = 125;
  const uint8_t startY = 170; // 170 makes centered without reset data
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.setColor(MINI_BLUE);
  gfx.drawString(labelX, startY + line * 15, label);
  gfx.setTextAlignment(TEXT_ALIGN_LEFT);
  gfx.setColor(MINI_WHITE);
  gfx.drawString(valueX, startY + line * 15, value);
}
