#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "time.h"
#include "FreeSerifBoldItalic20pt7b.h"
#include "FreeSerifBoldItalic14pt7b.h"
#include "FreeSerifBoldItalic11pt7b.h"
// ─── Broches ────────────────────────────────────────────────
#define BTN1     16
#define BTN2     17
#define BUZZER   32
#define RFID_CS  15
#define RFID_RST 26
#define SD_CS    33
#define TFT_CS   5
#define BTN_PRESSED  LOW
#define BTN_RELEASED HIGH
bool btn1Pressed = false;
bool btn2Pressed = false;
bool badgeScannedBtn1 = false; 
bool badgeScannedBtn2 = false; 
unsigned long scanTime = 0;
bool waitingRelease = false;
bool loadingDone = false;
bool travailEnCours = false;
bool projectIsPending = false;
int scanId = 0;
int currentScanId = 0;
int pendingDecisionScanId = 0;
String scannedButton = "";
String lastScannedUid = "";
String lockedUid = "";
String lockedButton = "";
String projetNom = "";
String tempsRestant = "";
String tempsEstime = "";
String checklistData = "";
String nextProjectName = "";

int taskState[4] = {0,0,0,0};
String pendingTasksMessage = "";
unsigned long lastDisplayTime = 0;
unsigned long long projectDueTimestamp = 0;
unsigned long lastCountdownUpdate = 0;
unsigned long ignoreButtonsUntil = 0;
const unsigned long displayCooldown = 500; // ms
void sendMQTT(String uid, String btn);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void setRGB(int rgbIndex, bool r, bool g, bool b);
void drawProjectDynamicData();
// ─── Réseau & MQTT ──────────────────────────────────────────
const char* ssid          = "TOPNET_E24C";
const char* password      = "RU3T39AVU2F2";
const char* mqtt_server = "192.168.100.10";
const uint16_t mqttPort   = 1883;
const char* mqttTopicScan = "pointage/action";
const char* mqttTopicButton   = "pointage/button";
const char* mqttTopicDecision = "pointage/decision";
const char* mqttTopicStatus   = "pointage/status";
const char* mqttTopicFinish = "pointage/finish";
const char* mqttTopicProject = "pointage/project";
const char* mqttTopicTasks = "pointage/tasks";
const char* mqttTopicSyncReq = "pointage/sync_request";
const char* mqttTopicFinishResult = "pointage/finish_result";
const char* mqttTopicNextProject = "pointage/next_project";

// ─── NTP ────────────────────────────────────────────────────
const char* ntpServer        = "pool.ntp.org";
const long  gmtOffset_sec    = 3600;
const int   daylightOffset_sec = 0;
// ─── Timing ─────────────────────────────────────────────────
const unsigned long scanDebounceMs   = 2000;
const unsigned long responseTimeoutMs = 8000;
const unsigned long resultScreenMs   = 3000;
const unsigned long unknownScreenMs  = 4000;
const unsigned long wifiRetryMs      = 5000;
const unsigned long mqttRetryMinMs   = 3000;   // back-off min
const unsigned long mqttRetryMaxMs   = 30000;  // back-off max
const unsigned long clockRefreshMs   = 1000;

// ─── États d'écran ──────────────────────────────────────────
enum ScreenState {
  SCREEN_MAIN,
  SCREEN_WAITING,
  SCREEN_RESULT,
  SCREEN_UNKNOWN
};
int loadingProgress = 0;
unsigned long lastLoadingUpdate = 0;
unsigned long btn1ReleaseTime = 0;
unsigned long btn2ReleaseTime = 0;

bool btn1WaitingCheck = false;
bool btn2WaitingCheck = false;
// ─── Objets globaux ─────────────────────────────────────────
MFRC522     rfid(RFID_CS, RFID_RST);
TFT_eSPI    tft = TFT_eSPI();
WiFiClient  mqttWifiClient;
PubSubClient client(mqttWifiClient);
bool projectScreenActive = false;
Adafruit_MCP23X17 mcp;

// ─── État machine ───────────────────────────────────────────
ScreenState screenState      = SCREEN_MAIN;
unsigned long screenStateSince = 0;
unsigned long lastWifiAttempt  = 0;
unsigned long lastMqttAttempt  = 0;
unsigned long mqttRetryInterval = mqttRetryMinMs; // [STAB] back-off
unsigned long lastClockRefresh = 0;
unsigned long lastScanAt       = 0;

bool waitingForResponse = false;
unsigned long waitingSince = 0;
unsigned long errorDisplayStart = 0;
bool showingError = false;

// [BUG-1] lastScannedUid correctement déclarée ici (était absente / placée après
//         du code orphelin qui utilisait déjà cette variable → erreur de compilation)
String lastScannedName = "";
String scanTimeString  = "";
String lastTime        = "";
String lastDate        = "";

// ─── Décision MQTT (accès depuis ISR/callback) ───────────────
volatile bool pendingDecision    = false;
String pendingDecisionUid        = "";
String pendingDecisionState      = "";

// ─── Buzzer ─────────────────────────────────────────────────
bool buzzerActive  = false;
bool buzzerLevel   = false;
unsigned long buzzerUntil    = 0;
unsigned long buzzerToggleAt = 0;

// ─── Boutons ────────────────────────────────────────────────
bool btnReady  = false;
bool lastBtn1  = HIGH;
bool lastBtn2  = HIGH;

// ─── Buffers BMP (hors stack pour éviter reset/crash) ───────
 const int bmpRowsPerChunk = 4;
 const int rgbPins[4][3] = {

  {0, 1, 2},     // RGB1 → GPA0 GPA1 GPA2
  {3, 4, 5},     // RGB2 → GPA3 GPA4 GPA5
  {6, 7, 8},     // RGB3 → GPA6 GPA7 GPB0
  {9, 10, 11}    // RGB4 → GPB1 GPB2 GPB3

};
uint8_t  bmpSDBuffer[240 * 3 * bmpRowsPerChunk];
uint16_t bmpLineBuffer[240 * bmpRowsPerChunk];

// ============================================================
//  Gestion SPI / CS
// ============================================================

void disableAll() {
  digitalWrite(SD_CS,   HIGH);
  digitalWrite(TFT_CS,  HIGH);
  digitalWrite(RFID_CS, HIGH);
}

void selectTFT() {
  digitalWrite(RFID_CS, HIGH);
  digitalWrite(SD_CS,   HIGH);  
  digitalWrite(TFT_CS,  LOW);
}

void selectSD() {
  digitalWrite(RFID_CS, HIGH);
  digitalWrite(TFT_CS,  HIGH);
  digitalWrite(SD_CS,   LOW);
}

void selectRFID() {
  digitalWrite(TFT_CS,  HIGH);
  digitalWrite(SD_CS,   HIGH);
  digitalWrite(RFID_CS, LOW);
}
// [BUG-2] L'accolade } parasite présente ici dans l'original a été supprimée.

// ============================================================
//  Lecture BMP depuis SD
// ============================================================

uint16_t read16(File& f) {
  uint16_t r;
  f.read((uint8_t*)&r, 2);
  return r;
}

uint32_t read32(File& f) {
  uint32_t r;
  f.read((uint8_t*)&r, 4);
  return r;
}

void drawBmp(const char* filename) {
  SPI.setFrequency(10000000);   // ← SD à 10MHz avant toute lecture
  selectSD();
  File bmpFile = SD.open(filename);
  if (bmpFile) {
  Serial.print("[DEBUG] File size: ");
  Serial.println(bmpFile.size());
  uint8_t buf[4];
  bmpFile.read(buf, 4);
  Serial.printf("[DEBUG] Header bytes: %02X %02X %02X %02X\n", buf[0], buf[1], buf[2], buf[3]);
  bmpFile.seek(0);  // remettre au début
}
  if (!bmpFile) {
    Serial.print("[TFT] BMP open failed: ");
    Serial.println(filename);

    disableAll();
    return;
  }

  if (read16(bmpFile) != 0x4D42) {

    Serial.println("[TFT] Invalid BMP header");

    bmpFile.close();
    disableAll();
    return;
  }

  (void)read32(bmpFile);
  (void)read32(bmpFile);

  uint32_t offset = read32(bmpFile);

  (void)read32(bmpFile);

  int w = (int)read32(bmpFile);
  int h = (int)read32(bmpFile);

  if (
      w > 240 ||
      h > 320 ||
      read16(bmpFile) != 1 ||
      read16(bmpFile) != 24
  ) {

    Serial.println("[TFT] Unsupported BMP format");

    bmpFile.close();

    disableAll();
    return;
  }

  bmpFile.seek(offset);

  int padding =
      (4 - (w * 3) % 4) % 4;

  // =====================================================
  // Lecture BMP par blocs
  // =====================================================

  for (
      int row = 0;
      row < h;
      row += bmpRowsPerChunk
  ) {

    int chunkRows = bmpRowsPerChunk;

    if (row + chunkRows > h)
        chunkRows = h - row;

    // =========================
    // Lecture SD
    // =========================

  // APRÈS
    selectSD();
    for (int r = 0; r < chunkRows; r++) {
      uint8_t* rowPtr = &bmpSDBuffer[r * w * 3];
      bmpFile.read(rowPtr, w * 3);
      if (padding) {
        bmpFile.seek(bmpFile.position() + padding);
      }
    }
    digitalWrite(SD_CS, HIGH);

    for (int r = 0; r < chunkRows; r++) {

      for (int col = 0; col < w; col++) {

        uint8_t* px =
            &bmpSDBuffer[(r * w + col) * 3];

        bmpLineBuffer[r * w + col] =
            tft.color565(
                px[2],
                px[1],
                px[0]
            );
      }
    }


    // APRÈS
    SPI.setFrequency(40000000);   // ← TFT à 40MHz avant écriture
    selectTFT();
    tft.startWrite();

    for (int r = 0; r < chunkRows; r++) {

      tft.pushImage(
          0,
          h - 1 - (row + r),
          w,
          1,
          &bmpLineBuffer[r * w]
      );

      yield();
    }

    tft.endWrite();
    disableAll();
  }
 // APRÈS
  bmpFile.close();
  disableAll();
  // ← rien d'autre, SD reste valide
}
void drawTaskTick(int x, int y, int size, uint16_t color) {
  int cx = x + size / 2;  
  int cy = y + size / 2;  
  int ax = cx - size / 3;
  int ay = cy;
  int bx = cx - size / 8;
  int by = cy + size / 4;

  // Branche droite du tick (centre-bas → haut-droite)
  int dx = cx + size / 3;
  int dy = cy - size / 4;

  // Dessiner épais (5px)
  for (int t = -2; t <= 2; t++) {
    tft.drawLine(ax, ay + t, bx, by + t, color);
    tft.drawLine(bx, by + t, dx, dy + t, color);
  }
}

void showMainScreen() {
  drawBmp("/a.bmp");
  projectScreenActive = false;
  travailEnCours      = false;
  screenState      = SCREEN_MAIN;
  screenStateSince = millis();
  loadingDone = false;
  lastTime         = "";
  lastDate         = "";
}
void drawLoadingBar(int progress) {
  selectTFT();

  int barWidth = 200;
  int barHeight = 8;

  // centre écran
  int x = (240 - barWidth) / 2;
  int y = (320 - barHeight) / 2;

  // fond
  // fond arrondi (gris)
 tft.fillRoundRect(x, y, barWidth, barHeight, 3, TFT_LIGHTGREY);

 // barre bleue arrondie
 int width = map(progress, 0, 100, 0, barWidth);
 tft.fillRoundRect(x, y, width, barHeight, 3, TFT_BLUE);

  // contour
  tft.drawRect(x, y, barWidth, barHeight, TFT_BLACK);

  // 🔥 TEXTE SOUS LA BARRE
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK);
  tft.setFreeFont(&FreeSerifBoldItalic14pt7b);

  tft.drawString("Lecture en cours...", 120, y + barHeight + 20);

  disableAll();
}
// APRÈS
void showLoading() {
  drawBmp("/b.bmp");
  disableAll();                  // ← AJOUT : état propre avant animation
  for (int i = 0; i <= 100; i += 3) {
    drawLoadingBar(i);
    // client.loop() retiré ici — trop risqué pendant animation SPI
    yield();
    delay(20);
  }
  int duration = 500;
  int steps = 50;
  for (int i = 0; i <= steps; i++) {
    int progress = map(i, 0, steps, 0, 100);
    drawLoadingBar(progress);
    yield();
    delay(duration / steps);
  }

 for (int i = 0; i <= steps; i++) {
  int progress = map(i, 0, steps, 0, 100);
  drawLoadingBar(progress);
  client.loop();
  yield();
  delay(duration / steps);
}
}


String readUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

bool getCurrentTime(String& timeValue) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  char buf[10];
  strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
  timeValue = String(buf);
  return true;
}


void drawResultOverlay() {
  selectTFT();

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK);

  // ✅ Font plus petite
  tft.setFreeFont(&FreeSerifBoldItalic11pt7b);

  // ---- Texte dynamique ----
  String line1 = "Bienvenue " + lastScannedName;
  String line2 = scanTimeString;

  int centerX = tft.width() / 2;
  int y = 80;       // point de départ
  int spacing = 50; // espace entre lignes

  // ---- Ligne 1 ----
  tft.drawString(line1, centerX, y);

  // ---- Ligne 2 ----
  tft.drawString(line2, centerX, y + (spacing * 2) + 20);

  disableAll();
}

void startBuzzer(unsigned long durationMs) {
  buzzerActive    = true;
  buzzerUntil     = millis() + durationMs;
  buzzerToggleAt  = 0;
}

void updateBuzzer() {
  if (!buzzerActive) return;
  // [STAB] Guard contre buzzerUntil == 0 au démarrage
  if (buzzerUntil == 0) { buzzerActive = false; return; }

  unsigned long now = millis();
  if (now >= buzzerUntil) {
    buzzerActive = false;
    buzzerLevel  = false;
    digitalWrite(BUZZER, LOW);
    return;
  }
  if (now >= buzzerToggleAt) {
    buzzerLevel = !buzzerLevel;
    digitalWrite(BUZZER, buzzerLevel ? HIGH : LOW);
    buzzerToggleAt = now + 120;
  }
}

void sendButtonStatus(const String& uid, const String& action) {
  if (!client.connected()) return;
  String payload = uid + "|" + action;
  bool ok = client.publish(mqttTopicButton, payload.c_str());
  Serial.print("[BTN] ");
  Serial.print(payload);
  Serial.println(ok ? " -> sent" : " -> publish-failed");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
if (length == 0) return;

String message;

for (unsigned int i = 0; i < length; i++) {
message += (char)payload[i];
}

if (String(topic) == mqttTopicDecision) {

  int p1 = message.indexOf('|');
  int p2 = message.indexOf('|', p1 + 1);
  int p3 = message.indexOf('|', p2 + 1);

  if (p1 < 0 || p2 < 0 || p3 < 0) {
    Serial.println("[MQTT] decision format invalide");
    return;
  }

  String incomingUid     = message.substring(0, p1);
  String incomingName    = message.substring(p1 + 1, p2);
  String incomingState   = message.substring(p2 + 1, p3);
  String incomingScanId  = message.substring(p3 + 1);

  incomingUid.trim();
  incomingState.trim();

  if (incomingUid != lastScannedUid) {
    Serial.println("[MQTT] UID mismatch — ignoré");
    return;
  }

  pendingDecisionUid = incomingUid;
  pendingDecisionState = incomingState;
  pendingDecisionScanId = incomingScanId.toInt();

  lastScannedName = incomingName;

  pendingDecision = true;

  Serial.println("[MQTT] Decision reçue: " + incomingState);

  return;
}

if (String(topic) == mqttTopicTasks) {
int f1=0, f2=0, f3=0, f4=0;

sscanf(message.c_str(),
       "%d|%d|%d|%d",
       &f1, &f2, &f3, &f4);

// cache
taskState[0]=f1;
taskState[1]=f2;
taskState[2]=f3;
taskState[3]=f4;

Serial.println("[TASKS] " + message);

// si f.bmp pas encore affiché
// APRÈS
if (!projectScreenActive) return;

// Redessiner le BMP comme fond propre puis poser les ticks actifs
drawBmp("/f.bmp");
if (projetNom.length() > 0) drawProjectDynamicData();
selectTFT();
if (f1==1) drawTaskTick(22,133,20,TFT_GREEN);
if (f2==1) drawTaskTick(22,168,20,TFT_GREEN);
if (f3==1) drawTaskTick(22,203,20,TFT_GREEN);
if (f4==1) drawTaskTick(22,238,20,TFT_GREEN);
disableAll();

// reset LEDs
for (int i=0; i<4; i++) {
  setRGB(i,false,false,false);
}

// rallumer bons RGB
if (f1==1) setRGB(0,false,false,true);
if (f2==1) setRGB(1,true,false,false);
if (f3==1) setRGB(2,false,true,false);
if (f4==1) setRGB(3,false,false,true);

return;

}
// =========================
// FINISH RESULT
if (String(topic) == mqttTopicFinishResult) {

  Serial.println("[MQTT] Finish result reçu: " + message);

  // ✅ allumer les 4 RGB
  for (int i = 0; i < 4; i++) {
    setRGB(i, false, true, false);
  }

  delay(3000);

  // ✅ éteindre LEDs
  for (int i = 0; i < 4; i++) {
    setRGB(i, false, false, false);
  }

  // ── Attendre nextProjectName max 5s ─────────────
  nextProjectName = "";

  unsigned long waitNext = millis();

  while (nextProjectName.length() == 0
         && millis() - waitNext < 5000) {

    client.loop();
    yield();
    delay(50);
  }

  Serial.println(
    "[FINISH] nextProjectName reçu: '"
    + nextProjectName + "'"
  );

  // ✅ afficher BMP résultat
  if (message == "g") {
    drawBmp("/g.bmp");
  } else {
    drawBmp("/h.bmp");
  }

  screenState = SCREEN_WAITING;

  // ✅ afficher prochain projet
  selectTFT();

  tft.setFreeFont(&FreeSerifBoldItalic11pt7b);

  tft.setTextColor(TFT_BLACK);

  tft.setTextSize(1);

  tft.setCursor(20, 240);

  if (nextProjectName.length() > 0) {
    tft.print(nextProjectName);
  } else {
    tft.print("Aucun projet");
  }

  // ✅ reset COMPLET ancien projet
  travailEnCours = false;

  projectScreenActive = false;
  showingError = false;
  projetNom = "";
  tempsEstime = "";
  nextProjectName = "";
  projectDueTimestamp = 0;
  // ✅ reset tâches
  for (int i = 0; i < 4; i++) {
    taskState[i] = 0;
  }
  // ✅ reset LEDs
  disableAll();
  // ✅ attendre écran résultat
  unsigned long waitBmp = millis();
  while (millis() - waitBmp < 30000) {
    yield();
    delay(10);
}

  // ✅ retour écran principal propre
  showMainScreen();
  return;
}
// =========================
// NEXT PROJECT
// =========================
// =========================
// NEXT PROJECT
// =========================

if (String(topic) == mqttTopicNextProject) {

  nextProjectName = message;

  nextProjectName.trim();

  // ✅ afficher seulement pendant écran finish
  if (screenState == SCREEN_WAITING) {

    Serial.println(
      "[NEXT PROJECT] '" +
      nextProjectName +
      "'"
    );
  }

  return;
}
// =========================
// PROJECT
// =========================
if (String(topic) == mqttTopicProject) {

  int p1 = message.indexOf('|');
  int p2 = message.indexOf('|', p1 + 1);

  if (p1 < 0 || p2 < 0)
    return;

  projetNom = message.substring(0, p1);
  tempsEstime = message.substring(p1 + 1, p2);

  String statusStr = "";

  int p3 = message.indexOf('|', p2 + 1);

  if (p3 > 0) {

    projectDueTimestamp =
      strtoull(
        message.substring(p2 + 1, p3).c_str(),
        NULL,
        10
      );

    statusStr =
      message.substring(p3 + 1);

  } else {

    projectDueTimestamp =
      strtoull(
        message.substring(p2 + 1).c_str(),
        NULL,
        10
      );
  }

  statusStr.trim();

  projectIsPending =
    (statusStr == "pending");

  Serial.println(
    "[PROJECT] " +
    projetNom +
    " | pending=" +
    String(projectIsPending)
  );

  // ✅ refresh automatique TFT
  // APRÈS
  if (travailEnCours && projectScreenActive) {
    // f.bmp déjà affiché → juste redessiner les données par-dessus
    drawProjectDynamicData();
    redrawTasksFromCache();
  }
  // si travailEnCours mais pas encore projectScreenActive
  // → processPendingDecision s'en occupe via le while() ci-dessus
  return;
}
}
void ensureWifiConnected() {
  static bool wifiStarted = false;
  if (WiFi.status() == WL_CONNECTED)
      return;
  if (!wifiStarted) {
    Serial.println("[WiFi] Connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    wifiStarted = true;
  }
}

void ensureMqttConnected() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (client.connected()) {
    mqttRetryInterval = mqttRetryMinMs;
    return;
  }
  unsigned long now = millis();
  if (now - lastMqttAttempt < mqttRetryInterval) return;
  lastMqttAttempt   = now;
  mqttRetryInterval = min(mqttRetryInterval * 2UL, mqttRetryMaxMs);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char clientId[32];
  snprintf(clientId, sizeof(clientId),
           "esp32-%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.print("[MQTT] Connecting to ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.print(mqttPort);
  Serial.print(" as ");
  Serial.println(clientId);

  bool ok = client.connect(
  clientId,
  mqttTopicStatus,
  0,
  true,
  "offline"
);
  if (!ok) {
    const char* reason = "UNKNOWN";
    switch (client.state()) {
      case -4: reason = "TIMEOUT — broker injoignable";        break;
      case -3: reason = "CONNECTION_LOST";                     break;
      case -2: reason = "CONNECT_FAILED — voir mosquitto.conf"; break;
      case -1: reason = "DISCONNECTED";                        break;
      case  1: reason = "BAD_PROTOCOL";                        break;
      case  2: reason = "BAD_CLIENT_ID";                       break;
      case  3: reason = "SERVER_UNAVAILABLE";                  break;
      case  4: reason = "BAD_CREDENTIALS";                     break;
      case  5: reason = "UNAUTHORIZED";                        break;
    }
    Serial.print("[MQTT] Failed: ");
    Serial.print(reason);
    Serial.print(" — retry in ");
    Serial.print(mqttRetryInterval / 1000);
    Serial.println("s");
    return;
  }

  // APRÈS
  mqttRetryInterval = mqttRetryMinMs;

  // ✅ Effacer tous les retained AVANT de s'abonner
  // → empêche de recevoir un projet retained d'une session précédente
  client.publish(mqttTopicFinishResult, "", true);
  client.publish(mqttTopicFinish,       "", true);
  client.publish(mqttTopicProject,      "", true);  // ← AJOUT
  client.publish(mqttTopicTasks,        "", true);  // ← AJOUT
  delay(150);  // ← légèrement plus long pour que le broker traite les effacements

  client.subscribe(mqttTopicDecision);
  client.subscribe(mqttTopicFinish);
  client.subscribe(mqttTopicProject);
  client.subscribe(mqttTopicTasks);
  client.subscribe(mqttTopicFinishResult);
  client.subscribe(mqttTopicNextProject);
  client.publish(mqttTopicStatus, "online", true);

  Serial.println("[MQTT] Connected + subscribed OK");
  client.publish(mqttTopicFinishResult, "", true);
  client.publish(mqttTopicFinish,       "", true);
}
// APRÈS — tick direct sur BMP, sans fillRect, sans carré
void redrawTasksFromCache() {
  selectTFT();
  if (taskState[0]==1) drawTaskTick(22, 133, 20, TFT_GREEN);
  if (taskState[1]==1) drawTaskTick(22, 168, 20, TFT_GREEN);
  if (taskState[2]==1) drawTaskTick(22, 203, 20, TFT_GREEN);
  if (taskState[3]==1) drawTaskTick(22, 238, 20, TFT_GREEN);
  disableAll();
  for (int i=0; i<4; i++) setRGB(i, false, false, false);
  if (taskState[0]==1) setRGB(0, false, false, true);
  if (taskState[1]==1) setRGB(1, true,  false, false);
  if (taskState[2]==1) setRGB(2, false, true,  false);
  if (taskState[3]==1) setRGB(3, true, true, true);
}
void processPendingDecision() {
  if (!pendingDecision) return;
  pendingDecision = false;

  if (pendingDecisionScanId != 0 && pendingDecisionScanId != currentScanId) {
    Serial.println("[FLOW] Ancienne réponse ignorée (scanId mismatch)");
    return;
  }

  waitingForResponse = false;

  Serial.print("[FLOW] Decision: ");
  Serial.println(pendingDecisionState);

  if (pendingDecisionState == "ok" || pendingDecisionState == "late") {

    if (pendingDecisionState == "ok") drawBmp("/e.bmp");
    else                              drawBmp("/d.bmp");
    drawResultOverlay();

    unsigned long waitStart = millis();
    while (millis() - waitStart < 5000) {
      client.loop();
      updateBuzzer();
      pendingDecision = false;
    }

    pendingDecision = false;

    // APRÈS — attendre que projetNom arrive, max 5s
    // APRÈS (bloc ok/late) — attendre d'abord, dessiner ensuite
    // ← attendre projetNom AVANT drawBmp (SPI libre pour client.loop)
    // APRÈS
    if (projetNom.length() == 0) {
      unsigned long t = millis();
      while (projetNom.length() == 0 && millis() - t < 5000) {
        client.loop();
        yield();
        delay(50);
      }
    }
    Serial.println("[FLOW] projetNom: '" + projetNom + "'");
    drawBmp("/f.bmp");
    projectScreenActive = true;
    projectIsPending = false;

   drawProjectDynamicData();
  // Attendre que les tâches arrivent si pas encore reçues
  // APRÈS — attendre que taskState soit non-nul, max 4s
unsigned long tWait = millis();
while (millis() - tWait < 4000) {
  if (taskState[0] || taskState[1] || taskState[2] || taskState[3]) break;
  client.loop();
  yield();
  delay(30);
}
Serial.printf("[TASKS] cache final: %d %d %d %d\n",
  taskState[0], taskState[1], taskState[2], taskState[3]);
redrawTasksFromCache();
  
    screenState        = SCREEN_RESULT;
    screenStateSince   = millis();
    travailEnCours     = true;
    showingError       = false;
    waitingRelease     = false;
    btn1Pressed        = false;
    btn2Pressed        = false;
    ignoreButtonsUntil = millis() + 1000;
    lastBtn1           = digitalRead(BTN1);
    lastBtn2           = digitalRead(BTN2);

  }
  else if (pendingDecisionState == "already") {

  Serial.println("[FLOW] Deja pointe");

// APRÈS (bloc already) — même logique
  // APRÈS
  if (projetNom.length() == 0) {
    unsigned long t = millis();
    while (projetNom.length() == 0 && millis() - t < 5000) {
      client.loop();
      yield();
      delay(50);
    }
  }
  Serial.println("[FLOW] projetNom: '" + projetNom + "'");

  // APRÈS (bloc already)
  // APRÈS
   for (int i = 0; i < 4; i++) taskState[i] = 0;  // ← reset avant réception
   drawBmp("/f.bmp");
   projectScreenActive = true;
   projectIsPending = false;

  drawProjectDynamicData();

// Attendre que les tâches arrivent si pas encore reçues
// APRÈS — attendre que taskState soit non-nul, max 4s
unsigned long tWait = millis();
while (millis() - tWait < 4000) {
  if (taskState[0] || taskState[1] || taskState[2] || taskState[3]) break;
  client.loop();
  yield();
  delay(30);
}
Serial.printf("[TASKS] cache final: %d %d %d %d\n",
  taskState[0], taskState[1], taskState[2], taskState[3]);
redrawTasksFromCache();
  startBuzzer(300);

  waitingRelease    = false;
  btn1Pressed       = false;
  btn2Pressed       = false;
  lastBtn1          = digitalRead(BTN1);
  lastBtn2          = digitalRead(BTN2);

  screenState       = SCREEN_RESULT;
  screenStateSince  = millis();
  travailEnCours = true;
  showingError   = false;

  }
  else {

  drawBmp("/c.bmp");

  startBuzzer(1000);

  waitingRelease    = false;
  btn1Pressed       = false;
  btn2Pressed       = false;
  lastBtn1          = digitalRead(BTN1);
  lastBtn2          = digitalRead(BTN2);

  screenState       = SCREEN_UNKNOWN;
  screenStateSince  = millis();
  showingError      = true;
  errorDisplayStart = millis();
  }
  }
  void updateProjectCountdown() {

  if (!travailEnCours || !projectScreenActive)
    return;

  if (projectIsPending) {

    selectTFT();

    tft.fillRect(115, 98, 110, 22, 0xC638);

    tft.setTextColor(TFT_BLUE);
    tft.setFreeFont(&FreeSerifBoldItalic11pt7b);
    tft.setCursor(118, 114);

    // afficher temps estimé fixe
    long estimatedSeconds = tempsEstime.toInt();

    int days = estimatedSeconds / 86400;
    int hours = (estimatedSeconds % 86400) / 3600;

    char estBuffer[20];

    sprintf(estBuffer, "%dj %dh", days, hours);

    tft.print(estBuffer);

    disableAll();

    return;
}
  if (!travailEnCours || screenState != SCREEN_RESULT)
    return;

  if (millis() - lastCountdownUpdate < 1000)
      return;

  lastCountdownUpdate = millis();

  unsigned long long nowMs =
      (unsigned long long)time(nullptr) * 1000ULL;

  long long diff =
      (long long)projectDueTimestamp -
      (long long)nowMs;

  if (diff <= 0) {

  tempsRestant = "00:00:00";
  selectTFT();
  tft.fillRect(115, 98, 110, 22, 0xC638);
  tft.setTextColor(TFT_RED);
  tft.setFreeFont(&FreeSerifBoldItalic11pt7b);
  tft.setCursor(118, 114);
  tft.print("00:00:00");
  disableAll();
  return;
}

  long totalSeconds = diff / 1000;

  int hours = totalSeconds / 3600;

  int minutes =
      (totalSeconds % 3600) / 60;

  int seconds =
      totalSeconds % 60;

  char buffer[20];

  sprintf(
    buffer,
    "%02d:%02d:%02d",
    hours,
    minutes,
    seconds
  );

  tempsRestant = String(buffer);

  selectTFT();
  tft.fillRect(115, 98, 110, 22, 0xC638);
  tft.setTextColor(TFT_RED);

  tft.setFreeFont(&FreeSerifBoldItalic11pt7b);

  tft.setCursor(118, 114);

  tft.print(tempsRestant);

  disableAll();
}
void updateScreenState() {
  unsigned long now = millis();
  if (travailEnCours) return;
  if (screenState == SCREEN_WAITING && waitingForResponse &&
      (now - waitingSince >= responseTimeoutMs)) {
    waitingForResponse = false;
    showMainScreen();
    Serial.println("[FLOW] Timeout waiting decision");
  }

  if (!travailEnCours &&
    screenState == SCREEN_RESULT &&
    (now - screenStateSince >= resultScreenMs)) {

    showMainScreen();

} else if (screenState == SCREEN_UNKNOWN &&
           (now - screenStateSince >= unknownScreenMs)) {

    showMainScreen();
}
}


void pollButtons() {
  bool currentBtn1 = digitalRead(BTN1);
  bool currentBtn2 = digitalRead(BTN2);

  if (!btnReady) {
    btnReady = true;
    lastBtn1 = currentBtn1;
    lastBtn2 = currentBtn2;
    return;
  }

  if (millis() < ignoreButtonsUntil) {
    lastBtn1 = currentBtn1;   
    lastBtn2 = currentBtn2;
    return;
  }
  if (travailEnCours) {
    bool btn1JustReleased =(lastBtn1 == BTN_PRESSED && currentBtn1 == BTN_RELEASED);
    bool btn2JustReleased =(lastBtn2 == BTN_PRESSED && currentBtn2 == BTN_RELEASED);

    if (btn1JustReleased || btn2JustReleased) {
      Serial.println("[BTN] Bouton pressé pendant travail → retour écran principal");
      travailEnCours = false;
      showingError   = false;
      btn1Pressed    = false;
      btn2Pressed    = false;
      lastBtn1       = currentBtn1;
      lastBtn2       = currentBtn2;
      showMainScreen();
      return;
    }

    lastBtn1 = currentBtn1;
    lastBtn2 = currentBtn2;
    return;
  }

  if (lastBtn1 == BTN_PRESSED && currentBtn1 == BTN_RELEASED) {
    Serial.println("[BTN1] Relâché (classeur retiré)");
    btn1Pressed = false;

    if (waitingRelease && scannedButton == "btn1") {
      if (millis() - scanTime <= 30000) {
        Serial.println("[SYSTEM] ➜ Envoi serveur (BTN1)");
        waitingRelease     = false;
        sendMQTT(lockedUid, "btn1");
        waitingForResponse = true;
        waitingSince       = millis();
      } else {
        Serial.println("[SYSTEM] ❌ Timeout 30s");
        drawBmp("/c.bmp");
        startBuzzer(1000);
        showingError      = true;
        errorDisplayStart = millis();
      }
    } else {
      if (!waitingRelease) {
        Serial.println("[BTN1] ❌ Relâché sans scan");
        drawBmp("/c.bmp");
        startBuzzer(1000);
        showingError      = true;
        errorDisplayStart = millis();
      }
    }
  }

  if (lastBtn1 == BTN_RELEASED && currentBtn1 == BTN_PRESSED) {
    Serial.println("[BTN1] Pressé (classeur posé)");
    btn1Pressed = true;
  }

  // ===== BTN2 =====
  if (lastBtn2 == BTN_PRESSED && currentBtn2 == BTN_RELEASED) {
    Serial.println("[BTN2] Relâché (classeur retiré)");
    btn2Pressed = false;

    if (waitingRelease && scannedButton == "btn2") {
      if (millis() - scanTime <= 30000) {
        Serial.println("[SYSTEM] ➜ Envoi serveur (BTN2)");
        waitingRelease     = false;
        sendMQTT(lockedUid, "btn2");
        waitingForResponse = true;
        waitingSince       = millis();
      } else {
        Serial.println("[SYSTEM] ❌ Timeout 30s");
        drawBmp("/c.bmp");
        startBuzzer(1000);
        showingError      = true;
        errorDisplayStart = millis();
      }
    } else {
      if (!waitingRelease) {
        Serial.println("[BTN2] ❌ Relâché sans scan");
        drawBmp("/c.bmp");
        startBuzzer(1000);
        showingError      = true;
        errorDisplayStart = millis();
      }
    }
  }

  if (lastBtn2 == BTN_RELEASED && currentBtn2 == BTN_PRESSED) {
    Serial.println("[BTN2] Pressé (classeur posé)");
    btn2Pressed = true;
  }

  lastBtn1 = currentBtn1;
  lastBtn2 = currentBtn2;
}
void pollRfid() {

  if (screenState == SCREEN_RESULT || screenState == SCREEN_UNKNOWN) return;
  if (showingError) return;

  unsigned long now = millis();
  if ((now - lastScanAt) < scanDebounceMs) return;

  selectRFID();
  bool cardPresent = rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial();
  if (!cardPresent) {
    disableAll();
    return;
  }

  String uid = readUid();
  uid.replace(" ", "");
  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  disableAll();

  lastScanAt = now;

  Serial.print("[RFID] UID=");
  Serial.println(uid);
  if (waitingRelease) {
    if (uid == lockedUid) {
      return; 
    }
    Serial.println("[SECURITY] Badge différent ignoré");
    return;
  }

  // Associer bouton
  if (btn1Pressed) {
    scannedButton = "btn1";
  }
  else if (btn2Pressed) {
    scannedButton = "btn2";
  }
  else {
    Serial.println("[RFID] ❌ Aucun bouton pressé");
    return;
  }
  lockedUid = uid;
  lockedButton = scannedButton;

  lastScannedUid = uid;
  lastScannedName = "En attente...";

  if (!getCurrentTime(scanTimeString)) scanTimeString = "--:--";
  scanId++;
  currentScanId = scanId;
  waitingRelease = true;
  scanTime = millis();

  Serial.println("[RFID] Scan OK → attente relâchement bouton");

  drawBmp("/b.bmp");
  showLoading();  
  screenState = SCREEN_WAITING;
  screenStateSince = now;
  waitingForResponse = false;
}
void drawProjectDynamicData() {

  selectTFT();

  tft.setTextDatum(TL_DATUM);

  tft.setFreeFont(&FreeSerifBoldItalic11pt7b);
  tft.setTextColor(TFT_BLUE);
  tft.setCursor(42, 58);
  if (tempsEstime.length() == 0 || projetNom.length() == 0) {
  tft.print("...");
  } else {
  long estimatedSeconds = tempsEstime.toInt();
  int days = estimatedSeconds / 86400;
  int hours = (estimatedSeconds % 86400) / 3600;
  char estBuffer[20];
  sprintf(estBuffer, "%dj %dh", days, hours);
  tft.print(estBuffer);
}

tft.setTextColor(TFT_BLUE);
tft.setCursor(110, 86);
tft.print(projetNom.length() == 0 ? "..." : projetNom);
  disableAll();
}
void updateClockOnMainScreen() {
  if (screenState != SCREEN_MAIN) return;
  unsigned long nowMs = millis();
  if (nowMs - lastClockRefresh < clockRefreshMs) return;
  lastClockRefresh = nowMs;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char timeStr[10];
  char dateStr[20];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  strftime(dateStr, sizeof(dateStr), "%d/%m/%Y", &timeinfo);

  String newTime = String(timeStr);
  String newDate = String(dateStr);
  if (newTime == lastTime && newDate == lastDate) return;

  selectTFT();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSerifBoldItalic20pt7b);
  tft.drawString(newTime, 120, 95);
  tft.setFreeFont(&FreeSerifBoldItalic14pt7b);
  tft.drawString(newDate, 120, 135);
  disableAll();

  lastTime = newTime;
  lastDate = newDate;
}
void setRGB(int rgbIndex, bool r, bool g, bool b) {
  mcp.digitalWrite(rgbPins[rgbIndex][0], r ? HIGH : LOW);
  mcp.digitalWrite(rgbPins[rgbIndex][1], g ? HIGH : LOW);
  mcp.digitalWrite(rgbPins[rgbIndex][2], b ? HIGH : LOW);
}
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  if (!mcp.begin_I2C()) {
    Serial.println("[MCP] not found");
  } else {
    Serial.println("[MCP] OK");
    for (int rgb = 0; rgb < 4; rgb++) {
      for (int c = 0; c < 3; c++) {
        mcp.pinMode(rgbPins[rgb][c], OUTPUT);
        mcp.digitalWrite(rgbPins[rgb][c], LOW);
      }
    }
  }

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);
  pinMode(SD_CS,   OUTPUT);
  pinMode(TFT_CS,  OUTPUT);
  pinMode(RFID_CS, OUTPUT);
  disableAll();  // tous CS HIGH

  // APRÈS — identique au code GitHub qui fonctionnait
  SPI.begin(18, 19, 23);
  SPI.setFrequency(10000000);

  // Init SD
  selectSD();                   // ← AJOUT : CS SD LOW, autres HIGH
  // Ajouter dans setup() après SD.begin() réussi
if (SD.begin(SD_CS)) {
  Serial.println("[SD] init ok");
  // Test d'ouverture directe
  selectSD();
  File test = SD.open("/a.bmp");
  if (test) {
    Serial.print("[SD] Test open OK, size=");
    Serial.println(test.size());
    test.close();
  } else {
    Serial.println("[SD] Test open FAILED dans setup");
  }
  digitalWrite(SD_CS, HIGH);
}

  // Init TFT
  selectTFT();                  // ← déjà présent, ok
  tft.init();
  SPI.end();
  delay(10);
  SPI.begin(18, 19, 23);
  SPI.setFrequency(40000000);
  tft.setSwapBytes(true);
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  disableAll();

  // RFID
  selectRFID();
  rfid.PCD_Init();
  disableAll();
  Serial.println("[RFID] ready");

  client.setServer(mqtt_server, mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);
  client.setKeepAlive(30);

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  showMainScreen();
  Serial.println("[BOOT] setup complete");
}
void loop() {
  ensureWifiConnected();
  ensureMqttConnected();
  client.loop();
  processPendingDecision();

  if (waitingRelease && (millis() - scanTime > 30000)) {
    Serial.println("[SYSTEM] Timeout 30s — bouton non relâché");
    drawBmp("/c.bmp");
    startBuzzer(1000);
    waitingRelease    = false;
    showingError      = true;
    errorDisplayStart = millis();
    screenState       = SCREEN_UNKNOWN;
    screenStateSince  = millis();
  }

  if (showingError && !travailEnCours &&
      (millis() - errorDisplayStart > 5000)) {
    Serial.println("[SYSTEM] Retour écran principal");
    showingError   = false;
    waitingRelease = false;
    showMainScreen();
  }

  updateScreenState();
  updateProjectCountdown();
  pollRfid();
  pollButtons();
  updateClockOnMainScreen();
  updateBuzzer();
}

void sendMQTT(String uid, String btn) {
  String payload = uid + "|" + btn + "|" + String(currentScanId);
  Serial.println("[MQTT] Envoi: " + payload);
  Serial.println("[DEBUG ESP32] UID=" + uid + " BTN=" + btn);

  if (client.connected()) {
    client.publish(mqttTopicScan, payload.c_str());
  } else {
    Serial.println("[MQTT] ❌ Non connecté");
  }
}