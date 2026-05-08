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
#define BTN_PRESSED  HIGH
#define BTN_RELEASED LOW
bool btn1Pressed = false;
bool btn2Pressed = false;
bool badgeScannedBtn1 = false; 
bool badgeScannedBtn2 = false; 
unsigned long scanTime = 0;
bool waitingRelease = false;
bool loadingDone = false;
bool travailEnCours = false;
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
const char* ssid          = "Cafe SEVEN";
const char* password      = "20262026";
const char* mqtt_server = "192.168.0.151";
const uint16_t mqttPort   = 1883;
const char* mqttTopicScan = "pointage/action";
const char* mqttTopicButton   = "pointage/button";
const char* mqttTopicDecision = "pointage/decision";
const char* mqttTopicStatus   = "pointage/status";
const char* mqttTopicFinish = "pointage/finish";
const char* mqttTopicProject = "pointage/project";
const char* mqttTopicTasks = "pointage/tasks";
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

  selectSD();

  File bmpFile = SD.open(filename);

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

    selectSD();

    for (int r = 0; r < chunkRows; r++) {

      uint8_t* rowPtr =
          &bmpSDBuffer[r * w * 3];

      bmpFile.read(rowPtr, w * 3);

      if (padding) {

        bmpFile.seek(
            bmpFile.position() + padding
        );
      }
    }

    digitalWrite(SD_CS, HIGH);

    // =========================
    // RGB888 → RGB565
    // =========================

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

    // =========================
    // Affichage TFT
    // =========================

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


  bmpFile.close();

  disableAll();
}
void drawTaskTick(int x, int y, int size, uint16_t color) {
  // ✅ Tick ✓ épais et visible dans la case
  int cx = x + size / 2;  // centre X de la case
  int cy = y + size / 2;  // centre Y de la case

  // Branche gauche du tick (bas-gauche → centre-bas)
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
// ============================================================
//  Écran principal
// ============================================================

void showMainScreen() {
  drawBmp("/a.bmp");
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
void showLoading() {
  drawBmp("/b.bmp");

  // animation 1 seconde
  for (int i = 0; i <= 100; i += 3) {
    drawLoadingBar(i);
    delay(20); // 1000ms total (1 seconde)
  }

 int duration = 500; // 0.5 seconde
 int steps = 50;

 for (int i = 0; i <= steps; i++) {
  int progress = map(i, 0, steps, 0, 100);
  drawLoadingBar(progress);
  delay(duration / steps);
}
}


// ============================================================
//  RFID helpers
// ============================================================

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

// ============================================================
//  Overlay résultat TFT
// ============================================================

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

// ============================================================
//  Buzzer (non-bloquant)
// ============================================================

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

// ============================================================
//  MQTT — publication boutons
// ============================================================

void sendButtonStatus(const String& uid, const String& action) {
  if (!client.connected()) return;
  String payload = uid + "|" + action;
  bool ok = client.publish(mqttTopicButton, payload.c_str());
  Serial.print("[BTN] ");
  Serial.print(payload);
  Serial.println(ok ? " -> sent" : " -> publish-failed");
}

// ============================================================
//  MQTT — callback décision serveur
//  Format attendu : UID|Nom Prenom|etat
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // ✅ Message construit EN PREMIER
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  // =========================
  // FINISH
  // =========================
  if (String(topic) == "pointage/finish") {
    Serial.println("[MQTT] Projet terminé");
    travailEnCours = false;
    showingError   = false;      // ✅ AJOUTÉ
    btn1Pressed    = false;      // ✅ AJOUTÉ
    btn2Pressed    = false;      // ✅ AJOUTÉ
    lastBtn1       = BTN_RELEASED; // ✅ AJOUTÉ
    lastBtn2       = BTN_RELEASED; // ✅ AJOUTÉ
    showMainScreen();
    return;
  }

  // =========================
  // TASKS
  // =========================
  if (String(topic) == "pointage/tasks") {
    Serial.println("[TASKS] " + message);

    int f1 = 0, f2 = 0, f3 = 0, f4 = 0;
    sscanf(message.c_str(), "%d|%d|%d|%d", &f1, &f2, &f3, &f4);

    // ── Coordonnées exactes des cases sur f.bmp ───────────────
    const int BOX_X    = 22;  // ✅ coin gauche des cases
    const int BOX_SIZE = 20;  // ✅ taille case
    const int Y1 = 133;       // ✅ Finaliser
    const int Y2 = 168;       // ✅ Vérifier
    const int Y3 = 203;       // ✅ Envoyer
    const int Y4 = 238;       // ✅ Commander

    selectTFT();

    // ✅ Redessiner tous les ticks cochés à chaque update
    if (f1 == 1) drawTaskTick(BOX_X, Y1, BOX_SIZE, TFT_GREEN);
    if (f2 == 1) drawTaskTick(BOX_X, Y2, BOX_SIZE, TFT_GREEN);
    if (f3 == 1) drawTaskTick(BOX_X, Y3, BOX_SIZE, TFT_GREEN);
    if (f4 == 1) drawTaskTick(BOX_X, Y4, BOX_SIZE, TFT_GREEN);

    disableAll();

    // ── RGB LEDs ──────────────────────────────────────────────
    for (int i = 0; i < 4; i++) setRGB(i, false, false, false);
    if (f1 == 1) setRGB(0, false, false, true);
    if (f2 == 1) setRGB(1, true,  false, false);
    if (f3 == 1) setRGB(2, false, true,  false);
    if (f4 == 1) setRGB(3, false, false, true);
    return;
  }

  // =========================
  // PROJET
  // =========================
  if (String(topic) == "pointage/project") {
    Serial.println("[MQTT PROJECT] " + message);

    int p1 = message.indexOf('|');
    int p2 = message.indexOf('|', p1 + 1);

    projetNom   = message.substring(0, p1);
    tempsEstime = message.substring(p1 + 1, p2);
    projectDueTimestamp = strtoull(
      message.substring(p2 + 1).c_str(), NULL, 10
    );

    Serial.println("[PROJECT] " + projetNom);
    return;
  }

  // =========================
  // DÉCISION POINTAGE
  // =========================
  Serial.println("[MQTT] " + message);

  int p1 = message.indexOf('|');
  int p2 = message.indexOf('|', p1 + 1);
  int p3 = message.lastIndexOf('|');

  if (p1 < 0 || p2 < 0) return;

  String uid  = message.substring(0, p1);        uid.trim();
  String name = message.substring(p1 + 1, p2);   name.trim();
  String state;
  int receivedScanId = 0;

  if (p3 == p2) {
    state = message.substring(p2 + 1);
  } else {
    state          = message.substring(p2 + 1, p3);
    receivedScanId = message.substring(p3 + 1).toInt();
  }

  state.trim();
  state.toLowerCase();

  Serial.println("[DEBUG] state=" + state + "=");

  pendingDecisionUid    = uid;
  pendingDecisionState  = state;
  pendingDecisionScanId = receivedScanId;
  lastScannedName       = name;
  pendingDecision       = true;
}
// ============================================================
//  Connectivité WiFi
// ============================================================

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

// ============================================================
//  Connectivité MQTT (avec back-off exponentiel) [STAB]
// ============================================================

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

  // MAC stable — évite les collisions de clientId sur le broker
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
    // Décodage du code d'erreur PubSubClient
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

  mqttRetryInterval = mqttRetryMinMs; // reset back-off
  client.subscribe(mqttTopicDecision);
  client.subscribe(mqttTopicFinish);
  client.subscribe(mqttTopicProject);
  client.subscribe(mqttTopicTasks);
  client.publish(mqttTopicStatus, "online", true);
  Serial.println("[MQTT] Connected + subscribed OK");
}
// ============================================================
//  Traitement de la décision reçue par MQTT
// ============================================================

// ============================================================
//  processPendingDecision() — version corrigée et complète
// ============================================================
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

    // ✅ Attente non-bloquante — pendingDecision vidé à chaque itération
    unsigned long waitStart = millis();
    while (millis() - waitStart < 5000) {
      client.loop();
      updateBuzzer();
      pendingDecision = false; // ✅ CRITIQUE : ignore tout message reçu pendant l'attente
      yield();
    }

    // ✅ Vider une dernière fois juste avant f.bmp
    pendingDecision = false;

    drawBmp("/f.bmp");
    drawProjectDynamicData();

    screenState        = SCREEN_RESULT;
    screenStateSince   = millis();
    travailEnCours     = true;
    showingError       = false;
    waitingRelease     = false;
    btn1Pressed        = false;
    btn2Pressed        = false;
    ignoreButtonsUntil = millis() + 1000; // ✅ 1s pour absorber relâchements résiduels
    lastBtn1           = digitalRead(BTN1);
    lastBtn2           = digitalRead(BTN2);

  } else {
    // "no" ou inconnu
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

  if (diff < 0)
      diff = 0;

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

  // ✅ Fenêtre d'immunité après transition d'écran
  if (millis() < ignoreButtonsUntil) {
    lastBtn1 = currentBtn1;   // mettre à jour silencieusement
    lastBtn2 = currentBtn2;
    return;
  }

  // ── Pendant travail (f.bmp) : bouton pressé → retour a.bmp ──
  if (travailEnCours) {
    bool btn1JustPressed = (lastBtn1 == BTN_RELEASED && currentBtn1 == BTN_PRESSED);
    bool btn2JustPressed = (lastBtn2 == BTN_RELEASED && currentBtn2 == BTN_PRESSED);

    if (btn1JustPressed || btn2JustPressed) {
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

  // ===== BTN1 =====
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

  // 🔒 Protection anti remplacement badge
  if (waitingRelease) {
    if (uid == lockedUid) {
      return; // même badge → ignore
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

  // 🔒 verrouillage
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

  // 🔵 Temps estimé
  tft.setTextColor(TFT_BLUE);

  tft.setCursor(42, 58);
  tft.print(tempsEstime);

  // ⚪ Projet
  tft.setTextColor(TFT_WHITE);

  tft.setCursor(110, 86);
  tft.print(projetNom);


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

  // CATHODE COMMUNE
  // HIGH = ON
  // LOW  = OFF

  mcp.digitalWrite(rgbPins[rgbIndex][0], r ? HIGH : LOW);
  mcp.digitalWrite(rgbPins[rgbIndex][1], g ? HIGH : LOW);
  mcp.digitalWrite(rgbPins[rgbIndex][2], b ? HIGH : LOW);
}
// ============================================================
//  setup()
// ============================================================

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

    // ✅ RGB éteint au démarrage
    mcp.digitalWrite(rgbPins[rgb][c], LOW);
  }
}
 }
  digitalWrite(BUZZER, LOW);
  disableAll();

  SPI.begin(18, 19, 23);
  SPI.setFrequency(10000000);
  // Init SD
  selectSD();
  if (!SD.begin(SD_CS)) {
    Serial.println("[SD] init failed");
  } else {
    Serial.println("[SD] init ok");
  }
  digitalWrite(SD_CS, HIGH);

  // Init TFT
  selectTFT();
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

  // MQTT
  client.setServer(mqtt_server, mqttPort);
  client.setCallback(mqttCallback);   // ✅ CORRIGÉ
  client.setBufferSize(256);
  client.setKeepAlive(30);

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  showMainScreen();
  Serial.println("[BOOT] setup complete");
}
void loop() {
  ensureWifiConnected();
  ensureMqttConnected();
  client.loop();

  // ✅ Ne traiter les décisions QUE si pas déjà en travail
  if (!travailEnCours) {
    processPendingDecision();
  } else {
    pendingDecision = false; // ✅ Jeter toute décision reçue pendant f.bmp
  }

  // ── Timeout bouton non relâché (30s) ─────────────────────
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

  // ── Retour écran principal après erreur (5s) ──────────────
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