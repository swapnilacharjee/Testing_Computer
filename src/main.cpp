#define ENABLE_LEGACY_TOKEN
#define ENABLE_DATABASE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PZEM004Tv30.h>
#include <FirebaseClient.h>
#include <time.h>

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
  Serial.println("No WiFi found! Restarting..."); delay(3000); ESP.restart();
}

// ---------------- FIREBASE ----------------
#define DATABASE_URL    "https://testing-pc-5d796-default-rtdb.asia-southeast1.firebasedatabase.app"
#define DATABASE_SECRET "r9B6FUMY9N1b4Nvs8iNuIuOH5qy67kPPHJNaazEM"

void asyncCB(AsyncResult &aResult);
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
LegacyToken legacy_token(DATABASE_SECRET);
FirebaseApp app;
RealtimeDatabase Database;

// ---------------- PZEM ----------------
#define RXD2 16
#define TXD2 17
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, RXD2, TXD2);

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
const float ON_THRESHOLD  = 10.0;   // ON threshold
const float OFF_THRESHOLD = 5.0;    // OFF threshold (hysteresis)
unsigned long stateChangeTime = 0;
bool pendingState = false;
bool hasPending   = false;
#define STATE_DEBOUNCE_MS 5000UL    // 5s debounce
#define UPDATE_INTERVAL 5000
bool updatePending = false;
int lastDay = -1;

// Reusable AsyncResult (global to avoid stack overflow)
AsyncResult gResult;

// ---------------- PRODUCTION ----------------
#define PRODUCTION_WATT_THRESHOLD 20.0f
#define MACHINE_WARMUP_MS 15000UL

unsigned long machineOnTime   = 0;   // millis() when machine turned ON
bool  warmupDone    = false;
bool  cuttingActive = false;
bool  prodDirty     = false;  // flag: production changed, needs Firebase update
long  todayProduction = 0;
long  totalProduction = 0;

// ---------------- TIME ----------------
void syncTime() {
  configTime(6 * 3600, 0, "time.google.com", "pool.ntp.org");
  Serial.print("Syncing time");
  int retry = 0;
  while (time(nullptr) < 1000000000 && retry < 40) { delay(250); Serial.print("."); retry++; }
  Serial.println(time(nullptr) < 1000000000 ? " FAILED" : " OK");
}

String getTimestamp() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[25];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

String getDateKey() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
  return String(buf);
}

void asyncCB(AsyncResult &aResult) {
  if (aResult.isError())
    Firebase.printf("Error [%s]: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
  updatePending = false;
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

  // Reset PZEM
  pzem.resetEnergy();
  delay(2000);
  float initE = pzem.energy();
  lastEnergy  = isnan(initE) ? 0 : initE;
  todayEnergy = 0;
  Serial.println("PZEM Reset! E=" + String(lastEnergy, 4));

  // Restore today production
  Database.get(aClient, "/PC_Monitor/production/" + getDateKey() + "/todaycuts", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) { long s = String(gResult.c_str()).toInt(); if (s > 0) { todayProduction = s; Serial.println("Restored today prod: " + String(s)); } }

  // Restore total production
  Database.get(aClient, "/PC_Monitor/production/totalcuts", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) { long s = String(gResult.c_str()).toInt(); if (s > 0) { totalProduction = s; Serial.println("Restored total prod: " + String(s)); } }

  // Restore today energy
  Database.get(aClient, "/PC_Monitor/energy_history/" + getDateKey() + "/kwh", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) { float s = String(gResult.c_str()).toFloat(); if (s > 0) { todayEnergy = s; Serial.println("Restored today energy: " + String(s, 4)); } }

  // Restore total energy
  Database.get(aClient, "/PC_Monitor/energy/total", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) { float s = String(gResult.c_str()).toFloat(); if (s > 0) { totalEnergy = s; Serial.println("Restored total energy: " + String(s, 4)); } }

  // Restore usage minutes
  Database.get(aClient, "/PC_Monitor/usage/" + getDateKey() + "/minutes", gResult);
  for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 5000;) { app.loop(); delay(50); }
  if (gResult.available()) { float s = String(gResult.c_str()).toFloat(); if (s > 0) { todayUsageMin = s; Serial.println("Restored usage: " + String(s, 1) + " min"); } }

  // Read initial power to set machine state
  delay(1000);
  float bootPower = pzem.power();
  if (isnan(bootPower)) bootPower = 0;
  pcState     = bootPower > ON_THRESHOLD;
  lastPcState = pcState;
  hasPending  = false;
  if (pcState) {
    machineOnTime = millis() - MACHINE_WARMUP_MS; // treat as already warmed up on reboot
    warmupDone    = true;
  }

  // If machine is ON, restore lastOn time from Firebase for accurate duration
  if (pcState) {
    Database.get(aClient, "/PC_Monitor/status/lastOn", gResult);
    for (unsigned long w = millis(); !gResult.isResult() && millis() - w < 3000;) { app.loop(); delay(50); }
    if (gResult.available()) {
      String lastOnStr = gResult.c_str();
      lastOnStr.replace("\"", "");
      struct tm tm = {};
      if (sscanf(lastOnStr.c_str(), "%d-%d-%d %d:%d:%d",
          &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
          &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        pcOnSinceUnix = mktime(&tm);
        Serial.println("Restored lastOn: " + lastOnStr);
      } else {
        pcOnSinceUnix = time(nullptr);
      }
    } else {
      pcOnSinceUnix = time(nullptr);
    }
  } else {
    pcOnSinceUnix = 0;
  }

  // Save to Firebase
  String ssidStr = WiFi.SSID();
  long unixNow   = time(nullptr);
  String json = "{";
  json += "\"esp32\":{\"wifi_ssid\":\"" + ssidStr + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"last_seen_unix\":" + String(unixNow) + ",\"last_seen\":\"" + getTimestamp() + "\"},";
  json += "\"status\":{\"pcState\":\"" + String(pcState ? "ON" : "OFF") + "\"}";
  json += "}";
  Database.update(aClient, "/PC_Monitor", object_t(json.c_str()), asyncCB, "bootTask");

  // Log boot event removed to reduce queue pressure

  Serial.println("Boot state: " + String(pcState ? "MACHINE ON" : "MACHINE OFF") + " (" + String(bootPower) + "W)");
}

// ---------------- LOOP ----------------
void loop() {
  app.loop();

  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi lost!"); connectWiFi(); return; }

  voltage = pzem.voltage();
  current = pzem.current();
  power   = pzem.power();
  energy  = pzem.energy();

  if (isnan(voltage)) voltage = 0;
  if (isnan(current)) current = 0;
  if (isnan(power))   power   = 0;

  // Track today energy (only increment, never jump, skip if PZEM returned bad value)
  if (!isnan(energy) && energy > 0) {
    if (energy > lastEnergy) {
      float diff = energy - lastEnergy;
      if (diff < 0.005f) {  // max 0.005 kWh (5Wh) per 2s = 9000W sanity check
        todayEnergy += diff;
        totalEnergy += diff;
      } else {
        Serial.printf("Energy spike ignored: %.4f kWh\n", diff);
      }
    }
    lastEnergy = energy;
  }
  if (isnan(energy)) energy = 0;

  // Midnight reset
  time_t nowT = time(nullptr);
  struct tm* t = localtime(&nowT);
  int today = t->tm_mday;
  if (lastDay == -1) lastDay = today;
  if (today != lastDay) {
    lastDay = today;
    todayEnergy   = 0;
    todayUsageMin = 0;
    todayProduction = 0;
    warmupDone    = false;
    cuttingActive = false;
    pzem.resetEnergy();
    lastEnergy    = 0;
    // totalEnergy is cumulative, do NOT reset on midnight
    String newDateKey = getDateKey();
    String midJson = "{\"energy\":{\"today\":0,\"today_cost\":0},\"production\":{\"" + newDateKey + "\":{\"todaycuts\":0}},\"usage\":{\"" + newDateKey + "\":{\"minutes\":0}}}";
    Database.update(aClient, "/PC_Monitor", object_t(midJson.c_str()), asyncCB, "midReset");
    Serial.println("Midnight reset!");
  }

  // Debounced state detection with hysteresis
  bool rawState = pcState ? (power > OFF_THRESHOLD) : (power > ON_THRESHOLD);
  if (rawState != pcState) {
    if (!hasPending || rawState != pendingState) {
      pendingState    = rawState;
      stateChangeTime = millis();
      hasPending      = true;
    } else if (millis() - stateChangeTime >= STATE_DEBOUNCE_MS) {
      pcState    = pendingState;
      hasPending = false;
    }
  } else {
    hasPending = false;
  }

  // State change
  if (pcState != lastPcState) {
    lastPcState = pcState;
    String ts = getTimestamp(), dateKey = getDateKey();
    if (pcState) {
      pcOnSinceUnix = time(nullptr);
      machineOnTime = millis();
      warmupDone    = false;
      cuttingActive = false;
      Serial.println("Machine ON at " + ts);
      if (app.ready()) {
        String evKey = String(time(nullptr));
        String onJson = "{\"status\":{\"pcState\":\"ON\",\"lastOn\":\"" + ts + "\"},\"events\":{\"" + dateKey + "\":{\"" + evKey + "\":{\"state\":\"ON\",\"time\":\"" + ts + "\",\"type\":\"machine\",\"desc\":\"Machine turned ON\"}}}}";
        Database.update(aClient, "/PC_Monitor", object_t(onJson.c_str()), asyncCB, "onTask");
      }
    } else {
      warmupDone    = false;
      cuttingActive = false;
      float usedMin = pcOnSinceUnix > 0 ? (time(nullptr) - pcOnSinceUnix) / 60.0 : 0;
      pcOnSinceUnix = 0;
      todayUsageMin += usedMin;
      Serial.printf("Machine OFF at %s (%.1f min)\n", ts.c_str(), usedMin);
      if (app.ready()) {
        String evKey = String(time(nullptr));
        String offJson = "{\"status\":{\"pcState\":\"OFF\",\"lastOff\":\"" + ts + "\"},\"usage\":{\"" + dateKey + "\":{\"minutes\":" + String(todayUsageMin, 1) + "}},\"events\":{\"" + dateKey + "\":{\"" + evKey + "\":{\"state\":\"OFF\",\"time\":\"" + ts + "\",\"type\":\"machine\",\"desc\":\"Machine turned OFF\",\"duration_min\":" + String(usedMin, 1) + "}}}}";
        Database.update(aClient, "/PC_Monitor", object_t(offJson.c_str()), asyncCB, "offTask");
      }
    }
  }

  // ---------------- PRODUCTION COUNTING ----------------
  if (pcState) {
    if (!warmupDone && (millis() - machineOnTime >= MACHINE_WARMUP_MS)) {
      warmupDone = true;
      Serial.println("Warmup done. Production counting active.");
    }

    if (warmupDone) {
      bool inCutZone = (power > PRODUCTION_WATT_THRESHOLD);

      if (!cuttingActive && inCutZone) {
        cuttingActive = true;
        todayProduction++;
        totalProduction++;
        prodDirty = true;
        Serial.printf("CUT! Today:%ld  Total:%ld\n", todayProduction, totalProduction);
      } else if (cuttingActive && !inCutZone) {
        cuttingActive = false;
      }
    }
  }

  // Regular update every 2s
  if (app.ready() && !updatePending && (millis() - lastUpdate > UPDATE_INTERVAL)) {
    lastUpdate = millis();
    updatePending = true;
    float todayCost = todayEnergy * 14.0;
    float totalCost = totalEnergy * 14.0;
    long  unixNow   = time(nullptr);
    String dateKey  = getDateKey();

    String json = "{";
    json += "\"live\":{\"voltage\":" + String(voltage, 2) + ",\"current\":" + String(current, 2) + ",\"power\":" + String(power, 2) + "},";
    json += "\"energy\":{\"today\":" + String(todayEnergy, 4) + ",\"today_cost\":" + String(todayCost, 2) + ",\"total\":" + String(totalEnergy, 4) + ",\"total_cost\":" + String(totalCost, 2) + "},";
    json += "\"production\":{\"todaycuts\":" + String(todayProduction) + ",\"totalcuts\":" + String(totalProduction) + "},";
    json += "\"esp32\":{\"last_seen_unix\":" + String(unixNow) + ",\"last_seen\":\"" + getTimestamp() + "\",\"wifi_ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    json += "}";
    Database.update(aClient, "/PC_Monitor", object_t(json.c_str()), asyncCB, "updateTask");
    prodDirty = false;

    if (millis() - lastHistUpdate > 30000) {
      lastHistUpdate = millis();
      String histJson = "{\"energy_history\":{\"" + dateKey + "\":{\"kwh\":" + String(todayEnergy, 4) + "}},\"production\":{\"" + dateKey + "\":{\"todaycuts\":" + String(todayProduction) + "}}}";
      Database.update(aClient, "/PC_Monitor", object_t(histJson.c_str()), asyncCB, "histTask");
    }

    Serial.printf("V:%.1f I:%.2f P:%.1f E:%.4f Today:%.4f Prod:%ld\n", voltage, current, power, energy, todayEnergy, todayProduction);
  }
}
