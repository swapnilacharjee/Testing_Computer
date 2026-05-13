#define ENABLE_LEGACY_TOKEN
#define ENABLE_DATABASE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PZEM004Tv30.h>
#include <FirebaseClient.h>
#include <time.h>

// ---------------- CONFIG ----------------
#define PC_ID       "pc2"           // pc1=Saon, pc2=Swapnil, pc3=Santo, pc4=PC4
#define PC_NAME     "Swapnil PC"
#define PZEM_ADDR   0x02            // Saon=0x01, Swapnil=0x02, Santo=0x03, PC4=0x04
#define DB_PATH     "/Office_Monitor/pc2"

// ---------------- WIFI ----------------
struct WiFiCred { const char* ssid; const char* pass; };
WiFiCred networks[] = {
  { "Hossain Group_2G", "01611560883" },
  { "MAIS Factory",     "01711560883@" }
};
const int networkCount = 2;

void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  for (int i = 0; i < networkCount; i++) {
    Serial.print("Trying: "); Serial.println(networks[i].ssid);
    WiFi.begin(networks[i].ssid, networks[i].pass);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(500); Serial.print("."); retry++; }
    if (WiFi.status() == WL_CONNECTED) { Serial.println("\nConnected! IP: " + WiFi.localIP().toString()); return; }
    WiFi.disconnect();
    delay(500);
  }
  Serial.println("No WiFi! Restarting..."); delay(3000); ESP.restart();
}

// ---------------- FIREBASE ----------------
#define DATABASE_URL    "https://ssd-room-default-rtdb.asia-southeast1.firebasedatabase.app"
#define DATABASE_SECRET "aHN0Qd5S91tqw2O97c1YpK8NrRCgyi2Lyf9iaVf4"

void asyncCB(AsyncResult &aResult);
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
LegacyToken legacy_token(DATABASE_SECRET);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult gResult;
bool updatePending = false;

void asyncCB(AsyncResult &aResult) {
  if (aResult.isError())
    Firebase.printf("Error [%s]: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
  updatePending = false;
}

// ---------------- PZEM ----------------
#define RXD2 16
#define TXD2 17
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, RXD2, TXD2, PZEM_ADDR);

// ---------------- VARIABLES ----------------
unsigned long lastUpdate     = 0;
unsigned long lastHistUpdate = 0;
time_t        pcOnSinceUnix  = 0;
float voltage, current, power, energy;
float lastEnergy    = 0;
float todayEnergy   = 0;
float totalEnergy   = 0;
float todayUsageMin = 0;
bool  pcState = false, lastPcState = false;
const float ON_THRESHOLD  = 10.0;
const float OFF_THRESHOLD = 5.0;
unsigned long stateChangeTime = 0;
bool pendingState = false, hasPending = false;
#define STATE_DEBOUNCE_MS 5000UL
#define UPDATE_INTERVAL   5000
bool pzemReady = false;
unsigned long pzemReadyTime = 0;
#define PZEM_SETTLE_MS 5000UL
int lastDay = -1;

// ---------------- TIME ----------------
void syncTime() {
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  Serial.print("Syncing time");
  int retry = 0;
  while (time(nullptr) < 1000000000 && retry < 40) { delay(250); Serial.print("."); retry++; }
  Serial.println(time(nullptr) < 1000000000 ? " FAILED" : " OK");
}

String getTimestamp() {
  time_t now = time(nullptr) + 6 * 3600;
  struct tm* t = gmtime(&now);
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

String getDateKey() {
  time_t now = time(nullptr) + 6 * 3600;
  struct tm* t = gmtime(&now);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
  return String(buf);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  connectWiFi();
  syncTime();

  ssl_client.setInsecure();
  initializeApp(aClient, app, getAuth(legacy_token), asyncCB, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  unsigned long waitStart = millis();
  while (!app.ready() && millis() - waitStart < 10000) { app.loop(); delay(50); }
  Serial.println("Firebase Ready!");

  // PZEM init
  delay(2000);
  float initE = pzem.energy();
  lastEnergy    = (!isnan(initE) && initE > 0) ? initE : 0;
  pzemReady     = false;
  pzemReadyTime = millis();
  Serial.println("PZEM init E=" + String(lastEnergy, 4));

  // Restore today energy
  gResult = AsyncResult();
  Database.get(aClient, String(DB_PATH) + "/energy/today", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) {
    float s = String(gResult.c_str()).toFloat();
    if (s > 0) { todayEnergy = s; Serial.println("Restored today energy: " + String(s, 4)); }
  }
  if (todayEnergy == 0) {
    gResult = AsyncResult();
    Database.get(aClient, String(DB_PATH) + "/energy_history/" + getDateKey() + "/kwh", gResult);
    for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
    if (gResult.available()) {
      float s = String(gResult.c_str()).toFloat();
      if (s > 0) { todayEnergy = s; Serial.println("Restored today energy from history: " + String(s, 4)); }
    }
  }

  // Restore total energy
  gResult = AsyncResult();
  Database.get(aClient, String(DB_PATH) + "/energy/total", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) {
    float s = String(gResult.c_str()).toFloat();
    if (s > 0) { totalEnergy = s; Serial.println("Restored total energy: " + String(s, 4)); }
  }
  if (totalEnergy < todayEnergy) totalEnergy = todayEnergy;

  // Restore usage minutes
  gResult = AsyncResult();
  Database.get(aClient, String(DB_PATH) + "/usage/" + getDateKey() + "/minutes", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) {
    float s = String(gResult.c_str()).toFloat();
    if (s > 0) { todayUsageMin = s; Serial.println("Restored usage: " + String(s, 1) + " min"); }
  }

  // Boot power
  delay(1000);
  float bootPower = pzem.power();
  if (isnan(bootPower)) bootPower = 0;
  pcState = lastPcState = bootPower > ON_THRESHOLD;
  hasPending = false;

  // Restore lastOn
  if (pcState) {
    gResult = AsyncResult();
    Database.get(aClient, String(DB_PATH) + "/status/lastOn", gResult);
    for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 3000;) { app.loop(); delay(50); }
    if (gResult.available()) {
      String s = gResult.c_str(); s.replace("\"", "");
      struct tm tm = {}; tm.tm_isdst = -1;
      if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        pcOnSinceUnix = mktime(&tm);
      } else pcOnSinceUnix = time(nullptr);
    } else pcOnSinceUnix = time(nullptr);
  }

  // Boot update
  String ssidStr = WiFi.SSID();
  long unixNow = time(nullptr);
  String json = "{";
  json += "\"esp32\":{\"wifi_ssid\":\"" + ssidStr + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"last_seen_unix\":" + String(unixNow) + ",\"last_seen\":\"" + getTimestamp() + "\"},";
  json += "\"status\":{\"pcState\":\"" + String(pcState ? "ON" : "OFF") + "\"}";
  json += "}";
  Database.update(aClient, DB_PATH, object_t(json.c_str()), asyncCB, "bootTask");

  Serial.println("Boot: " + String(pcState ? "ON" : "OFF") + " (" + String(bootPower) + "W)");
}

// ---------------- LOOP ----------------
void loop() {
  app.loop();

  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return; }

  voltage = pzem.voltage();
  current = pzem.current();
  power   = pzem.power();
  energy  = pzem.energy();

  if (isnan(voltage)) voltage = 0;
  if (isnan(current)) current = 0;
  if (isnan(power))   power   = 0;

  // Energy tracking
  if (!pzemReady) {
    if (millis() - pzemReadyTime >= PZEM_SETTLE_MS) {
      pzemReady = true;
      float e = pzem.energy();
      if (!isnan(e) && e > 0) lastEnergy = e;
      Serial.println("PZEM settled. lastEnergy=" + String(lastEnergy, 4));
    }
  } else if (!isnan(energy) && energy > 0) {
    if (energy > lastEnergy) {
      float diff = energy - lastEnergy;
      if (diff < 0.005f) { todayEnergy += diff; totalEnergy += diff; }
      else Serial.printf("Spike ignored: %.4f kWh\n", diff);
    }
    lastEnergy = energy;
  }
  if (isnan(energy)) energy = 0;

  // Midnight reset
  time_t nowT = time(nullptr) + 6 * 3600;
  struct tm* t = gmtime(&nowT);
  int today = t->tm_mday;
  if (lastDay == -1) lastDay = today;
  if (today != lastDay) {
    lastDay = today;
    todayEnergy = todayUsageMin = 0;
    pzem.resetEnergy(); lastEnergy = 0;
    String newDateKey = getDateKey();
    String midJson = "{\"energy\":{\"today\":0,\"today_cost\":0},\"usage\":{\"" + newDateKey + "\":{\"minutes\":0}}}";
    Database.update(aClient, DB_PATH, object_t(midJson.c_str()), asyncCB, "midReset");
    Serial.println("Midnight reset!");
  }

  // Debounced state detection
  bool rawState = pcState ? (power > OFF_THRESHOLD) : (power > ON_THRESHOLD);
  if (rawState != pcState) {
    if (!hasPending || rawState != pendingState) { pendingState = rawState; stateChangeTime = millis(); hasPending = true; }
    else if (millis() - stateChangeTime >= STATE_DEBOUNCE_MS) { pcState = pendingState; hasPending = false; }
  } else hasPending = false;

  // State change
  if (pcState != lastPcState) {
    lastPcState = pcState;
    String ts = getTimestamp(), dateKey = getDateKey();
    String evKey = String(time(nullptr));
    if (pcState) {
      pcOnSinceUnix = time(nullptr);
      Serial.println("PC ON at " + ts);
      if (app.ready()) {
        String json = "{\"status\":{\"pcState\":\"ON\",\"lastOn\":\"" + ts + "\"},\"events\":{\"" + dateKey + "\":{\"" + evKey + "\":{\"state\":\"ON\",\"time\":\"" + ts + "\",\"type\":\"pc\",\"desc\":\"" + String(PC_NAME) + " turned ON\"}}}}";
        Database.update(aClient, DB_PATH, object_t(json.c_str()), asyncCB, "onTask");
      }
    } else {
      float usedMin = pcOnSinceUnix > 0 ? (time(nullptr) - pcOnSinceUnix) / 60.0 : 0;
      pcOnSinceUnix = 0;
      todayUsageMin += usedMin;
      Serial.printf("PC OFF at %s (%.1f min)\n", ts.c_str(), usedMin);
      if (app.ready()) {
        String json = "{\"status\":{\"pcState\":\"OFF\",\"lastOff\":\"" + ts + "\"},\"usage\":{\"" + dateKey + "\":{\"minutes\":" + String(todayUsageMin, 1) + "}},\"events\":{\"" + dateKey + "\":{\"" + evKey + "\":{\"state\":\"OFF\",\"time\":\"" + ts + "\",\"type\":\"pc\",\"desc\":\"" + String(PC_NAME) + " turned OFF\",\"duration_min\":" + String(usedMin, 1) + "}}}}";
        Database.update(aClient, DB_PATH, object_t(json.c_str()), asyncCB, "offTask");
      }
    }
  }

  // Regular update every 5s
  if (app.ready() && !updatePending && (millis() - lastUpdate > UPDATE_INTERVAL)) {
    lastUpdate = millis();
    updatePending = true;
    float todayCost = todayEnergy * 15.0;
    float totalCost = totalEnergy * 15.0;
    long  unixNow   = time(nullptr);
    String dateKey  = getDateKey();
    float runningMin = (pcState && pcOnSinceUnix > 0) ? (time(nullptr) - pcOnSinceUnix) / 60.0 : 0;
    float totalUsageMin = todayUsageMin + runningMin;

    String json = "{";
    json += "\"live\":{\"voltage\":" + String(voltage, 2) + ",\"current\":" + String(current, 2) + ",\"power\":" + String(power, 2) + "},";
    json += "\"energy\":{\"today\":" + String(todayEnergy, 4) + ",\"today_cost\":" + String(todayCost, 2) + ",\"total\":" + String(totalEnergy, 4) + ",\"total_cost\":" + String(totalCost, 2) + "},";
    json += "\"usage\":{\"" + dateKey + "\":{\"minutes\":" + String(totalUsageMin, 1) + "}},";
    json += "\"esp32\":{\"last_seen_unix\":" + String(unixNow) + ",\"last_seen\":\"" + getTimestamp() + "\",\"wifi_ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    json += "}";
    Database.update(aClient, DB_PATH, object_t(json.c_str()), asyncCB, "updateTask");

    // 30s history save
    if (millis() - lastHistUpdate > 30000) {
      lastHistUpdate = millis();
      String histJson = "{\"energy_history\":{\"" + dateKey + "\":{\"kwh\":" + String(todayEnergy, 4) + "}},\"energy\":{\"today\":" + String(todayEnergy, 4) + ",\"total\":" + String(totalEnergy, 4) + "}}";
      Database.update(aClient, DB_PATH, object_t(histJson.c_str()), asyncCB, "histTask");
    }

    Serial.printf("V:%.1f I:%.2f P:%.1f E:%.4f Today:%.4f Usage:%.1f\n", voltage, current, power, energy, todayEnergy, totalUsageMin);
  }
}
