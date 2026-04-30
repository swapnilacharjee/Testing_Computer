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
float todayUsageMin = 0;
bool  pcState = false, lastPcState = false;
const float ON_THRESHOLD = 10.0;
#define UPDATE_INTERVAL 2000
int lastDay = -1;

// ---------------- PRODUCTION ----------------
#define CUT_AMP_LOW    0.22f
#define CUT_AMP_HIGH   0.28f
#define IDLE_AMP_LOW   0.12f
#define IDLE_AMP_HIGH  0.17f
#define MACHINE_WARMUP_MS 15000UL

unsigned long machineOnTime   = 0;   // millis() when machine turned ON
bool  warmupDone    = false;
bool  cuttingActive = false;
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
    Firebase.printf("Error: %s, code: %d\n", aResult.error().message().c_str(), aResult.error().code());
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

  // Restore today production from Firebase
  AsyncResult todayProdResult;
  Database.get(aClient, "/PC_Monitor/production/" + getDateKey() + "/count", todayProdResult);
  unsigned long todayProdWait = millis();
  while (!todayProdResult.isResult() && millis() - todayProdWait < 5000) { app.loop(); delay(50); }
  if (todayProdResult.available()) {
    long saved = String(todayProdResult.c_str()).toInt();
    if (saved > 0) { todayProduction = saved; Serial.println("Restored today production: " + String(todayProduction)); }
  }

  // Restore total production from Firebase
  AsyncResult totalProdResult;
  Database.get(aClient, "/PC_Monitor/production/total", totalProdResult);
  unsigned long totalProdWait = millis();
  while (!totalProdResult.isResult() && millis() - totalProdWait < 5000) { app.loop(); delay(50); }
  if (totalProdResult.available()) {
    long saved = String(totalProdResult.c_str()).toInt();
    if (saved > 0) { totalProduction = saved; Serial.println("Restored total production: " + String(totalProduction)); }
  }

  // Read today's energy from Firebase
  AsyncResult energyResult;
  Database.get(aClient, "/PC_Monitor/energy_history/" + getDateKey() + "/kwh", energyResult);
  unsigned long bootWait = millis();
  while (!energyResult.isResult() && millis() - bootWait < 5000) { app.loop(); delay(50); }
  if (energyResult.available()) {
    float savedKwh = String(energyResult.c_str()).toFloat();
    if (savedKwh > 0) { todayEnergy = savedKwh; Serial.println("Restored energy: " + String(todayEnergy, 4)); }
  }

  // Read today's usage minutes from Firebase
  AsyncResult usageRestoreResult;
  Database.get(aClient, "/PC_Monitor/usage/" + getDateKey() + "/minutes", usageRestoreResult);
  unsigned long usageRestoreWait = millis();
  while (!usageRestoreResult.isResult() && millis() - usageRestoreWait < 5000) { app.loop(); delay(50); }
  if (usageRestoreResult.available()) {
    float savedMin = String(usageRestoreResult.c_str()).toFloat();
    if (savedMin > 0) { todayUsageMin = savedMin; Serial.println("Restored usage: " + String(todayUsageMin, 1) + " min"); }
  }

  // Read initial power to set machine state
  delay(1000);
  float bootPower = pzem.power();
  if (isnan(bootPower)) bootPower = 0;
  pcState     = bootPower > ON_THRESHOLD;
  lastPcState = pcState;
  if (pcState) {
    machineOnTime = millis() - MACHINE_WARMUP_MS; // treat as already warmed up on reboot
    warmupDone    = true;
  }

  // If machine is ON, restore lastOn time from Firebase for accurate duration
  if (pcState) {
    AsyncResult lastOnResult;
    Database.get(aClient, "/PC_Monitor/status/lastOn", lastOnResult);
    unsigned long lastOnWait = millis();
    while (!lastOnResult.isResult() && millis() - lastOnWait < 3000) { app.loop(); delay(50); }
    if (lastOnResult.available()) {
      String lastOnStr = lastOnResult.c_str();
      lastOnStr.replace("\"", "");
      // Parse timestamp to unix time
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

  // Log boot event
  String bootJson = "{\"state\":\"ON\",\"time\":\"" + getTimestamp() + "\",\"type\":\"esp32\",\"desc\":\"ESP32 Online - WiFi: " + ssidStr + "\"}";
  Database.push<object_t>(aClient, "/PC_Monitor/events/" + getDateKey(), object_t(bootJson.c_str()), asyncCB, "bootEvent");

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
  if (isnan(energy))  energy  = 0;

  // Track today energy (only increment, never jump)
  if (energy > lastEnergy && (energy - lastEnergy) < 1.0) // max 1 kWh per 2s sanity check
    todayEnergy += (energy - lastEnergy);
  lastEnergy = energy;

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
    lastEnergy    = energy;
    warmupDone    = false;
    cuttingActive = false;
    pzem.resetEnergy();
    String newDateKey = getDateKey();
    Database.set<number_t>(aClient, "/PC_Monitor/energy/today",                          number_t(0, 4), asyncCB, "midResetE");
    Database.set<number_t>(aClient, "/PC_Monitor/energy/today_cost",                     number_t(0, 2), asyncCB, "midResetC");
    Database.set<number_t>(aClient, "/PC_Monitor/production/" + newDateKey + "/count",   number_t(0, 0), asyncCB, "midResetP");
    Database.set<number_t>(aClient, "/PC_Monitor/usage/"      + newDateKey + "/minutes", number_t(0, 1), asyncCB, "midResetU");
    Serial.println("Midnight reset!");
  }

  pcState = power > ON_THRESHOLD;

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
        String ev = "{\"state\":\"ON\",\"time\":\"" + ts + "\",\"type\":\"machine\",\"desc\":\"Machine turned ON\"}";
        Database.push<object_t>(aClient, "/PC_Monitor/events/" + dateKey, object_t(ev.c_str()), asyncCB, "onEvent");
        Database.set<String>(aClient, "/PC_Monitor/status/pcState", "ON", asyncCB, "stateTask");
        Database.set<String>(aClient, "/PC_Monitor/status/lastOn",  ts,   asyncCB, "lastOnTask");
      }
    } else {
      warmupDone    = false;
      cuttingActive = false;
      float usedMin = pcOnSinceUnix > 0 ? (time(nullptr) - pcOnSinceUnix) / 60.0 : 0;
      pcOnSinceUnix = 0;
      Serial.printf("Machine OFF at %s (%.1f min)\n", ts.c_str(), usedMin);
      if (app.ready()) {
        String ev = "{\"state\":\"OFF\",\"time\":\"" + ts + "\",\"type\":\"machine\",\"desc\":\"Machine turned OFF\",\"duration_min\":" + String(usedMin, 1) + "}";
        Database.push<object_t>(aClient, "/PC_Monitor/events/" + dateKey, object_t(ev.c_str()), asyncCB, "offEvent");
        Database.set<String>(aClient, "/PC_Monitor/status/pcState", "OFF", asyncCB, "stateTask");
        Database.set<String>(aClient, "/PC_Monitor/status/lastOff", ts,    asyncCB, "lastOffTask");

        // Add to accumulated usage
        todayUsageMin += usedMin;
        Database.set<number_t>(aClient, "/PC_Monitor/usage/" + dateKey + "/minutes", number_t(todayUsageMin, 1), asyncCB, "usageTask");
      }
    }
  }

  // ---------------- PRODUCTION COUNTING ----------------
  if (pcState) {
    // Check warmup
    if (!warmupDone && (millis() - machineOnTime >= MACHINE_WARMUP_MS)) {
      warmupDone = true;
      Serial.println("Warmup done. Production counting active.");
    }

    if (warmupDone) {
      bool inCutZone = (current >= CUT_AMP_LOW && current <= CUT_AMP_HIGH);

      if (!cuttingActive && inCutZone) {
        // Rising edge: cutting started
        cuttingActive = true;
      } else if (cuttingActive && !inCutZone) {
        // Falling edge: cut completed → count 1 production
        cuttingActive = false;
        todayProduction++;
        totalProduction++;
        String dateKey = getDateKey();
        Serial.printf("CUT! Today:%ld  Total:%ld\n", todayProduction, totalProduction);
        if (app.ready()) {
          Database.set<number_t>(aClient, "/PC_Monitor/production/" + dateKey + "/count", number_t(todayProduction, 0), asyncCB, "prodTodayTask");
          Database.set<number_t>(aClient, "/PC_Monitor/production/total",                 number_t(totalProduction, 0),  asyncCB, "prodTotalTask");
        }
      }
    }
  }

  // Regular update every 2s
  if (app.ready() && (millis() - lastUpdate > UPDATE_INTERVAL)) {
    lastUpdate = millis();
    float todayCost = todayEnergy * 14.0;
    long  unixNow   = time(nullptr);

    String json = "{";
    json += "\"live\":{\"voltage\":" + String(voltage, 2) + ",\"current\":" + String(current, 2) + ",\"power\":" + String(power, 2) + "},";
    json += "\"energy\":{\"today\":" + String(todayEnergy, 4) + ",\"today_cost\":" + String(todayCost, 2) + "},";
    json += "\"production\":{\"today\":" + String(todayProduction) + ",\"total\":" + String(totalProduction) + "},";
    json += "\"esp32\":{\"last_seen_unix\":" + String(unixNow) + ",\"last_seen\":\"" + getTimestamp() + "\",\"wifi_ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    json += "}";
    Database.update(aClient, "/PC_Monitor", object_t(json.c_str()), asyncCB, "updateTask");

    // Energy history every 30s
    if (millis() - lastHistUpdate > 30000) {
      lastHistUpdate = millis();
      Database.set<number_t>(aClient, "/PC_Monitor/energy_history/" + getDateKey() + "/kwh", number_t(todayEnergy, 4), asyncCB, "histTask");
    }

    Serial.printf("V:%.1f I:%.2f P:%.1f E:%.4f Today:%.4f Prod:%ld\n", voltage, current, power, energy, todayEnergy, todayProduction);
  }
}
