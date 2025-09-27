#include <Wire.h>
#include <Adafruit_PCF8574.h>
#include <LiquidCrystal_I2C.h>
#include <SparkFun_VL53L1X.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "time.h"
#include <Preferences.h>
#include <ArduinoJson.h>

// ==== Forward declaration ====
void aktifkanPakan(int porsi);
String getTimeISO8601();
unsigned long lastSensorPublish = 0;
const unsigned long sensorInterval = 2000; // 2 detik

Preferences prefs;  // <--- supaya prefs dikenali di seluruh program

// ================== Relay & Servo ==================
#define RELAY1_PIN 26   // Relay motor
#define RELAY2_PIN 27   // Relay pengganti servo

// ================== Konfigurasi WiFi & NTP ==================
const char* ssid     = "heheheheh";
const char* password = "iya bentar";
const char* ntpServer = "id.pool.ntp.org";
const long  gmtOffset_sec      = 7 * 3600;
const int   daylightOffset_sec = 0;

/* ------------------ MQTT CONFIG ------------------ */
const char* mqtt_server = "broker.hivemq.com"; // IP broker Mosquitto
const int mqtt_port = 1883;

// Topic
#define TOPIC_CONTROL   "feeder/control"
#define TOPIC_SCHEDULE  "feeder/schedule"
#define TOPIC_INFO      "feeder/info"

WiFiClient espClient;
PubSubClient client(espClient);

// --- Device ID ---
String deviceID;

// --- Fungsi buat ambil MAC (ID unik ESP32) ---
String getDeviceID() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[20];
  sprintf(id, "ESP32_%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(id);
}

// ================== LCD, PCF8574, Sensor Laser ==================
LiquidCrystal_I2C lcd(0x27, 16, 2);
#define PCF8574_ADDR 0x20
Adafruit_PCF8574 pcf;
SFEVL53L1X tofSensor;

const float minDistance = 3.0;
const float maxDistance = 50.0;

// ================== Keypad ==================
int rowPins[4] = {4, 5, 6, 7};
int colPins[4] = {0, 1, 2, 3};

char keys[4][4] = {
  {'D', '#', '0', '*'},
  {'C', '9', '8', '7'},
  {'B', '6', '5', '4'},
  {'A', '3', '2', '1'}
};

// ================== Custom Character LCD ==================
byte barFull[8]  = {B11111,B11111,B11111,B11111,B11111,B11111,B11111,B11111};
byte barEmpty[8] = {B00000,B00000,B00000,B00000,B00000,B00000,B00000,B00000};

// ================== Menu ==================
String menus[3]        = {"   Beri Pakan", "Tambahkan Jadwal", "   Info Pakan"};
int menuCount          = 3;
int menuIndex          = 0;

// porsi sebagai integer (disimpan & diproses sebagai int)
int porsiItems[]       = {500, 1000, 1500};
int porsiCount         = 3;
int currentPorsi       = 0;

// string tampilan (opsional — tetap tidak dipakai untuk penyimpanan)
 // String portionList[3]  = {"500  G", "1000 G", "1500 G"}; // tidak perlu dipakai

String jadwalMenuList[3] = {"  Lihat Jadwal", " Tambah Jadwal", "  Hapus Jadwal"};
int jadwalMenuIndex      = 0;
int jadwalTotalMenu      = 3;

struct Jadwal {
    int jam;
    int menit;
    int porsi;
    int lastRunDay;
    bool alreadyTriggered = false; // ✅ flag per menit
};

// **Pastikan deklarasi array & counter berada sebelum fungsi save/load**
Jadwal jadwalList[10];
int totalJadwal = 0;
int viewIndex   = 0;

void saveJadwal() {
  prefs.begin("jadwal", false);  // write mode
  prefs.putInt("total", totalJadwal);

  for (int i = 0; i < totalJadwal; i++) {
    String key = "j" + String(i);
    // simpan dalam format "HH,MM,PP"  contoh: "07,30,500"
    String value = String(jadwalList[i].jam) + "," +
                   String(jadwalList[i].menit) + "," +
                   String(jadwalList[i].porsi);
    prefs.putString(key.c_str(), value);
  }
  prefs.end();
}

void loadJadwal() {
  prefs.begin("jadwal", true); // read-only
  totalJadwal = prefs.getInt("total", 0);
  if (totalJadwal > 10) totalJadwal = 10; // safety

  for (int i = 0; i < totalJadwal; i++) {
    String key = "j" + String(i);
    String value = prefs.getString(key.c_str(), "");
    if (value != "") {
      int jam, menit, porsi;
      // parse "HH,MM,PP"
      sscanf(value.c_str(), "%d,%d,%d", &jam, &menit, &porsi);
      jadwalList[i].jam   = jam;
      jadwalList[i].menit = menit;
      jadwalList[i].porsi = porsi;
      jadwalList[i].lastRunDay = -1;
    } else {
      // jika data kosong, set default agar tidak muncul garbage
      jadwalList[i].jam = 0;
      jadwalList[i].menit = 0;
      jadwalList[i].porsi = 0;
      jadwalList[i].lastRunDay = -1;
    }
  }
  prefs.end();
}

  // fungsi publish biar ga ngulang-ngulang
template<size_t N>
void publishJSON(const char* topic, StaticJsonDocument<N>& doc) {
  char buffer[256];  // buffer tetap 256 (cukup untuk payload ringan)
  serializeJson(doc, buffer);
  client.publish(topic, buffer);
}

// ====== HANDLER CONTROL ======
void handleControl(JsonDocument& doc) {
    String action = doc["action"] | "";
    int portion   = doc["portion"] | 500;
    String source = doc["source"] | "";

    if (action == "play") {
        aktifkanPakan(portion);
    } 
    else if (action == "pause") {
        digitalWrite(RELAY1_PIN, LOW);
        digitalWrite(RELAY2_PIN, LOW);
    }

    // ⚡ Tidak usah publish balik kalau dari App
}

// ====== HANDLER SCHEDULE ======
void handleSchedule(JsonDocument& doc) {
  String action = doc["action"] | "";
  int h = doc["hour"] | 0;
  int m = doc["minute"] | 0;
  int p = doc["portion"] | 500;

  if (action == "ADD" && totalJadwal < 10) {
    jadwalList[totalJadwal].jam = h;
    jadwalList[totalJadwal].menit = m;
    jadwalList[totalJadwal].porsi = p;
    jadwalList[totalJadwal].lastRunDay = -1;
    totalJadwal++;
    saveJadwal();
  }
  else if (action == "REMOVE" && totalJadwal > 0) {
    totalJadwal--;
    saveJadwal();
  }

  // 📤 Publish balik ke MQTT (konfirmasi)
  StaticJsonDocument<256> res;
  //res["type"]      = "schedule";
  res["action"]    = action;
  res["hour"]      = h;
  res["minute"]    = m;
  res["portion"]   = p;
  res["timestamp"] = getTimeISO8601();
  res["source"]    = deviceID;
  publishJSON("feeder/schedule", res);
}

// ================== CALLBACK MQTT =================
void callback(char* topic, byte* payload, unsigned int length) {
    StaticJsonDocument<256> doc;
    deserializeJson(doc, payload, length);

    String action = doc["action"] | "";
    String type   = doc["type"]   | "";
    int portion   = doc["portion"] | 500;
    String source = doc["source"] | "";

    // === handle control ===
    if (String(topic) == "feeder/control" && action == "play") {
        if (source != deviceID) { 
            aktifkanPakan(portion);
        }
    }

    // ---------------- SCHEDULE ----------------
  if (String(topic) == "feeder/schedule") {
      String source = doc["source"] | "";
      if (source == deviceID) return; // abaikan pesan yang dikirim sendiri

      String action = doc["action"] | "";
      int h = doc["hour"] | 0;
      int m = doc["minute"] | 0;
      int p = doc["portion"] | 500;

      if (action == "ADD" && totalJadwal < 10) {
          jadwalList[totalJadwal].jam = h;
          jadwalList[totalJadwal].menit = m;
          jadwalList[totalJadwal].porsi = p;
          jadwalList[totalJadwal].lastRunDay = -1;
          totalJadwal++;
          saveJadwal();
      } 
      else if (action == "REMOVE" && totalJadwal > 0) {
          totalJadwal--;
          saveJadwal();
      }
  }
}

// ---------------- RECONNECT MQTT ----------------
void reconnect() {
  while (!client.connected()) {
    Serial.print("🔄 Menghubungkan MQTT...");
    if (client.connect("ESP32_Feeder")) {
      Serial.println("✅ Terhubung");
      client.subscribe("feeder/control");
      client.subscribe("feeder/schedule");
    } else {
      Serial.print("❌ Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" coba lagi 5 detik...");
      delay(5000);
    }
  }
}

String inputBuffer = "";
int lastHour = -1, lastMinute = -1;
int portionIndex   = 0;

// ================== Mode ==================
enum Mode {
  MODE_TITLE,
  MODE_MAIN_MENU,
  MODE_PORSI_MENU,
  MODE_INFO_PAKAN,
  MODE_JADWAL_MENU,
  MODE_INPUT_JAM,
  MODE_INPUT_PORSI,
  MODE_VIEW_JADWAL,
  MODE_DELETE_JADWAL,
  MODE_INFO_JADWAL
};

Mode currentMode = MODE_TITLE;

// ================== Fungsi Baca Tombol ==================
char detectKeyOnce() {
  for (int c = 0; c < 4; c++) {
    for (int i = 0; i < 4; i++) pcf.digitalWrite(colPins[i], HIGH);
    pcf.digitalWrite(colPins[c], LOW);

    for (int r = 0; r < 4; r++) {
      if (pcf.digitalRead(rowPins[r]) == LOW) {
        delay(50);
        while (pcf.digitalRead(rowPins[r]) == LOW);
        return keys[r][c];
      }
    }
  }
  return 0;
}

// ========== Deklarasi fungsi dulu ==========
void aktifkanPakan(int porsi);
String getTimeISO8601();
void showTitle();
bool connectWiFi(int maxRetry);
void handleKey(char key);
void runLaserProgram();
void checkAndRunSchedule();
void showMenu();
void handleMainMenuKey(char key);
void handlePorsiKey(char key);
void handleJadwalMenuKey(char key);
void handleInputJamKey(char key);
void handleInputPorsiKey(char key);
void handleViewJadwalKey(char key);
void handleDeleteJadwalKey(char key);
void showPorsiMenu();
void showJadwalMenu();
void selectJadwalMenu();
void showViewJadwal();
void showDeleteJadwal();

// FUNGSI WAKTU ISO8601
// =========================
String getTimeISO8601() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
  return String(buf);
}

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  Wire.begin();
  lcd.init();
  lcd.backlight();

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  if (!pcf.begin(PCF8574_ADDR, &Wire)) {
    lcd.clear(); lcd.print("PCF8574 Error!");
    while (1);
  }

  for (int c = 0; c < 4; c++) pcf.pinMode(colPins[c], OUTPUT);
  for (int r = 0; r < 4; r++) pcf.pinMode(rowPins[r], INPUT_PULLUP);

  lcd.createChar(0, barEmpty);
  lcd.createChar(1, barFull);

  if (tofSensor.begin() != 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sensor Lepas!");
    while (1);
  }
  tofSensor.startRanging();

  showTitle();
  loadJadwal(); // <-- ambil jadwal dari NVS saat startup

  // --- WiFi connect ---
  if (connectWiFi(5)) {
    Serial.println("✅ WiFi Connected");
    Serial.print("📡 IP ESP32: ");
    Serial.println(WiFi.localIP());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  } else {
    Serial.println("❌ WiFi Gagal Connect, restart ESP...");
    delay(2000);
    ESP.restart();
  }

  // --- Device ID ---
  deviceID = getDeviceID();
  Serial.println("Device ID: " + deviceID);

  // --- MQTT setup ---
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}
// === FUNGSI UNTUK MANUAL (via keypad/LCD) ===
void tombolManual(int portion) {
  aktifkanPakan(portion); // Jalankan feeding

  // kirim feedback ke MQTT karena ini aksi manual
  StaticJsonDocument<128> doc;
  doc["action"]  = "play";
  doc["portion"] = portion;
  doc["source"]  = deviceID;
  publishJSON("feeder/control", doc);
}



// ================== Loop ==================
void loop() {
  runLaserProgram();
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

// Cek status WiFi tiap 10 detik lewat Serial
static unsigned long lastWiFiCheck = 0;
if (millis() - lastWiFiCheck > 10000) {
  lastWiFiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi Terputus, mencoba reconnect...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  } else {
    Serial.println("📶 WiFi OK: " + WiFi.localIP().toString());
  }
}

  char key = detectKeyOnce();
  if (key) handleKey(key);

  if (currentMode == MODE_INFO_PAKAN) runLaserProgram();

  checkAndRunSchedule();
  delay(50);
}

// ================== WiFi ==================
bool connectWiFi(int maxRetries) {
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < maxRetries) {
    delay(1000);
    retries++;
  }
  return WiFi.status() == WL_CONNECTED;
}

// ================== Waktu ==================
void getCurrentTime(int &hour, int &minute) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    hour = 0; minute = 0;
    return;
  }
  hour   = timeinfo.tm_hour;
  minute = timeinfo.tm_min;
}

// ================== Cek & Jalankan Jadwal ==================
void checkAndRunSchedule() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;
  int d = timeinfo.tm_mday;  // hari dalam sebulan

  // -----------------------
  // Filter tambahan: hanya trigger sekali per menit
  static int lastCheckedHour = -1;
  static int lastCheckedMinute = -1;
  if (h == lastCheckedHour && m == lastCheckedMinute) return; // sudah dicek menit ini
  lastCheckedHour = h;
  lastCheckedMinute = m;
  // -----------------------

for (int i = 0; i < totalJadwal; i++) {
    if (jadwalList[i].jam == h &&
        jadwalList[i].menit == m &&
        !jadwalList[i].alreadyTriggered) {

        aktifkanPakan(jadwalList[i].porsi);
        jadwalList[i].alreadyTriggered = true;
    }

    // Reset flag saat menit berganti
    if (jadwalList[i].menit != m) {
        jadwalList[i].alreadyTriggered = false;
    }
  }
}

// ================== Pemberian Pakan (Dual Relay) ==================
void aktifkanPakan(int porsi) {
  int durasi = 0;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Memberi Pakan:");
  lcd.setCursor(0, 1);
  lcd.print(String(porsi) + " G");

  if      (porsi == 500)  durasi = 5000;   // 5 detik
  else if (porsi == 1000) durasi = 10000;  // 10 detik
  else if (porsi == 1500) durasi = 15000;  // 15 detik

  // Step 1: Hidupkan Relay1 (motor) dulu
  digitalWrite(RELAY1_PIN, HIGH);
  delay(1000);

  // Step 2: Hidupkan Relay2 (pembuka) bareng Relay1
  digitalWrite(RELAY2_PIN, HIGH);
  delay(durasi - 1000);

  // Step 3: Matikan semua relay
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  delay(500); // jeda safety

  showTitle();
  currentMode = MODE_TITLE;
}

// ================== Handle Tombol ==================
  void handleKey(char key) {
  switch(currentMode) {
    case MODE_TITLE:
      if (key == 'B') { currentMode = MODE_MAIN_MENU; menuIndex = 0; showMenu(); }
      break;

    case MODE_MAIN_MENU:   handleMainMenuKey(key); break;
    case MODE_PORSI_MENU:  handlePorsiKey(key); break;
    case MODE_INFO_PAKAN:  if (key == '*') {currentMode = MODE_MAIN_MENU; showMenu();} break;
    case MODE_JADWAL_MENU: handleJadwalMenuKey(key); break;
    case MODE_INPUT_JAM:   handleInputJamKey(key); break;
    case MODE_INPUT_PORSI: handleInputPorsiKey(key); break;
    case MODE_VIEW_JADWAL: handleViewJadwalKey(key); break;
    case MODE_DELETE_JADWAL: handleDeleteJadwalKey(key); break;
  }
}

// ================== Menu Utama ==================
void showMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("      Menu:");
  lcd.setCursor(0,1); lcd.print(menus[menuIndex]);
}

void handleMainMenuKey(char key) {
  if (key == 'C') {
    menuIndex--; if (menuIndex < 0) menuIndex = menuCount - 1;
    showMenu();
  }
  else if (key == 'D') {
    menuIndex++; if (menuIndex >= menuCount) menuIndex = 0;
    showMenu();
  }
  else if (key == '#') {
    if      (menuIndex == 0) { currentMode = MODE_PORSI_MENU; currentPorsi = 0; showPorsiMenu(); }
    else if (menuIndex == 1) { currentMode = MODE_JADWAL_MENU; jadwalMenuIndex = 0; showJadwalMenu(); }
    else if (menuIndex == 2) { currentMode = MODE_INFO_PAKAN; lcd.clear(); lcd.print("Info Pakan"); delay(1000); lcd.clear(); }
  }
  else if (key == '*') {
    currentMode = MODE_TITLE; showTitle();
  }
}

// ================== Porsi Manual ==================
void showPorsiMenu() {
  lcd.clear();
  lcd.setCursor(0,0); 
  lcd.print("Pilih Porsi:");
  lcd.setCursor(0,1);
  lcd.print(String(porsiItems[currentMode == MODE_INPUT_PORSI ? portionIndex : currentPorsi]) + " G");
}

void handlePorsiKey(char key) {
  if (key == 'C') {
    currentPorsi--; if (currentPorsi < 0) currentPorsi = porsiCount - 1;
    showPorsiMenu();
  }
  else if (key == 'D') {
    currentPorsi++; if (currentPorsi >= porsiCount) currentPorsi = 0;
    showPorsiMenu();
  }
  else if (key == '#') {
      tombolManual(porsiItems[currentPorsi]);
  }
  else if (key == '*') {
    currentMode = MODE_MAIN_MENU; showMenu();
  }
}

// ================== Info Pakan (Sensor) ==================
void runLaserProgram() {
   if (tofSensor.checkForDataReady()) {
        uint16_t distanceMM = tofSensor.getDistance();
        float distance = distanceMM / 10.0; // cm

        float percent = 100 * (maxDistance - distance) / (maxDistance - minDistance);
        if (percent > 100) percent = 100;
        if (percent < 0) percent = 0;

        tofSensor.clearInterrupt();

        unsigned long now = millis();
        if (now - lastSensorPublish >= sensorInterval) {
            lastSensorPublish = now;

            // Kirim MQTT
            StaticJsonDocument<128> doc;
            doc["percent"] = percent;
            doc["source"]  = deviceID;
            publishJSON("feeder/info", doc);

        }

        // Update LCD hanya kalau mode info pakan aktif
        if (currentMode == MODE_INFO_PAKAN) {
            lcd.setCursor(0,0); lcd.print("   Isi Pakan    ");
            int barLength = map(percent, 0, 100, 0, 12);
            lcd.setCursor(0,1);
            for (int i=0; i<12; i++) lcd.write(i < barLength ? 1 : 0);
            lcd.setCursor(13,1); lcd.print((int)percent); lcd.print("% ");
        }
    }
}

// ================== Title ==================
void showTitle() {
  lcd.clear();
  String title = "    Feedora";
  lcd.setCursor(0,0);
  for (int i=0; i<title.length(); i++) {
    lcd.print(title[i]);
    delay(80);
  }
  delay(1000);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("   Tekan Menu");
}

// ================== Menu Jadwal ==================
void showJadwalMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Jadwal Menu:");
  lcd.setCursor(0,1); lcd.print(jadwalMenuList[jadwalMenuIndex]);
}

void handleJadwalMenuKey(char key) {
  if (key == 'C') {
    jadwalMenuIndex++; if (jadwalMenuIndex >= jadwalTotalMenu) jadwalMenuIndex = 0;
    showJadwalMenu();
  }
  else if (key == 'D') {
    jadwalMenuIndex--; if (jadwalMenuIndex < 0) jadwalMenuIndex = jadwalTotalMenu - 1;
    showJadwalMenu();
  }
  else if (key == '#') {
    selectJadwalMenu();
  }
  else if (key == '*') {
    currentMode = MODE_MAIN_MENU; showMenu();
  }
}

void showDeleteJadwal() {
  lcd.clear();
  if (totalJadwal == 0) {
    lcd.setCursor(0, 0);
    lcd.print("Tidak ada jadwal");
    delay(1000);
    currentMode = MODE_JADWAL_MENU;
    showJadwalMenu();
    return;
  }

  lcd.setCursor(0, 0);
  lcd.print("Hapus Jadwal:");

  lcd.setCursor(0, 1);
  char buf[16];
  sprintf(buf, "%02d:%02d P:%d", 
          jadwalList[viewIndex].jam, 
          jadwalList[viewIndex].menit, 
          jadwalList[viewIndex].porsi);
  lcd.print(buf);
}

void selectJadwalMenu() {
  if (jadwalMenuIndex == 1) {
    currentMode = MODE_INPUT_JAM; inputBuffer = "";
    lcd.clear(); lcd.print("Input Jam (HHMM)");
  }
  else if (jadwalMenuIndex == 0) {
    if (totalJadwal == 0) {
      lcd.clear(); lcd.print("Belum Ada Jadwal"); delay(1500);
      showJadwalMenu();
    } else {
      currentMode = MODE_VIEW_JADWAL; viewIndex = 0;
      showViewJadwal();
    }
  }
  else if (jadwalMenuIndex == 2) {
    if (totalJadwal == 0) {
      lcd.clear(); lcd.print("Belum Ada Jadwal"); delay(1500);
      showJadwalMenu();
    } else {
      currentMode = MODE_DELETE_JADWAL; viewIndex = 0;
      showDeleteJadwal();
    }
  }
}

// ================== Input Jam ==================
void handleInputJamKey(char key) {
  if (key >= '0' && key <= '9') {
    if (inputBuffer.length() < 4) {
      inputBuffer += key;
      lcd.setCursor(0,1);
      lcd.print(inputBuffer);
    }

    if (inputBuffer.length() == 4) {
      int jam   = inputBuffer.substring(0,2).toInt();
      int menit = inputBuffer.substring(2,4).toInt();

      if (jam >= 0 && jam <= 23 && menit >= 0 && menit <= 59) {
        lastHour   = jam;
        lastMinute = menit;
        currentMode = MODE_INPUT_PORSI;
        portionIndex = 0;
        showPorsiMenu();
      } else {
        lcd.clear(); lcd.print("Input Salah!"); delay(1000);
        currentMode = MODE_JADWAL_MENU; showJadwalMenu();
      }
    }
  }
  else if (key == '*') {
    currentMode = MODE_JADWAL_MENU; showJadwalMenu();
  }
}

// ================== Input Porsi Jadwal ==================
void handleInputPorsiKey(char key) {
  if (key == 'C') {
    portionIndex--; 
    if (portionIndex < 0) portionIndex = porsiCount - 1;
    showPorsiMenu();
  }
  else if (key == 'D') {
    portionIndex++; 
    if (portionIndex >= porsiCount) portionIndex = 0;
    showPorsiMenu();
  }
  else if (key == '#') {
    if (totalJadwal < 10) {
      int h = lastHour;
      int m = lastMinute;
      int p = porsiItems[portionIndex];

      jadwalList[totalJadwal].jam   = h;
      jadwalList[totalJadwal].menit = m;
      jadwalList[totalJadwal].porsi = p; 
      jadwalList[totalJadwal].lastRunDay = -1;
      totalJadwal++;
      saveJadwal();

      // 📤 Publish ke MQTT
      StaticJsonDocument<256> res;
      //res["type"]      = "schedule";
      res["action"]    = "ADD";
      res["hour"]      = h;
      res["minute"]    = m;
      res["portion"]   = p;
      res["timestamp"] = getTimeISO8601();
      res["source"]    = deviceID;
      publishJSON("feeder/schedule", res);

      lcd.clear(); lcd.print("Jadwal Disimpan!"); delay(1500);
    }
    currentMode = MODE_JADWAL_MENU; 
    showJadwalMenu();
  }
  else if (key == '*') {
    currentMode = MODE_JADWAL_MENU;
    showJadwalMenu();
  }
}

// ================== View Jadwal ==================
void showViewJadwal() {
  loadJadwal();
  if (totalJadwal == 0) {
    lcd.clear(); lcd.print("Belum Ada Jadwal"); delay(1200);
    currentMode = MODE_JADWAL_MENU; showJadwalMenu();
    return;
  }

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(jadwalList[viewIndex].jam < 10 ? "0" : ""); lcd.print(jadwalList[viewIndex].jam);
  lcd.print(":");
  lcd.print(jadwalList[viewIndex].menit < 10 ? "0" : ""); lcd.print(jadwalList[viewIndex].menit);

  lcd.setCursor(0,1);
  lcd.print(String(jadwalList[viewIndex].porsi) + " G");
}

void handleViewJadwalKey(char key) {
  if (key == 'C') {
    viewIndex++; if (viewIndex >= totalJadwal) viewIndex = 0;
    showViewJadwal();
  }
  else if (key == 'D') {
    viewIndex--; if (viewIndex < 0) viewIndex = totalJadwal - 1;
    showViewJadwal();
  }
  else if (key == '*') {
    currentMode = MODE_JADWAL_MENU; showJadwalMenu();
  }
}

// ================== Delete Jadwal ==================
void handleDeleteJadwalKey(char key) {
  if (key == '#') {
    // simpan jadwal yang mau dihapus
    int h = jadwalList[viewIndex].jam;
    int m = jadwalList[viewIndex].menit;
    int p = jadwalList[viewIndex].porsi;

    // Hapus dari array
    for (int i = viewIndex; i < totalJadwal - 1; i++) {
      jadwalList[i] = jadwalList[i+1];
    }
    totalJadwal--;
    if (totalJadwal < 0) totalJadwal = 0;
    saveJadwal();

    // 📤 Publish ke MQTT data jadwal yang dihapus
    StaticJsonDocument<256> res;
    //res["type"]      = "schedule";
    res["action"]    = "REMOVE";
    res["hour"]      = h;
    res["minute"]    = m;
    res["portion"]   = p;
    res["timestamp"] = getTimeISO8601();
    res["source"]    = deviceID;
    publishJSON("feeder/schedule", res);

    // Navigasi LCD setelah hapus
    if (totalJadwal == 0) {
      currentMode = MODE_JADWAL_MENU; 
      showJadwalMenu();
      return;
    }
    if (viewIndex >= totalJadwal) viewIndex = 0;
    showDeleteJadwal();
  }
  else if (key == 'C') {
    viewIndex++; if (viewIndex >= totalJadwal) viewIndex = 0;
    showDeleteJadwal();
  }
  else if (key == 'D') {
    viewIndex--; if (viewIndex < 0) viewIndex = totalJadwal - 1;
    showDeleteJadwal();
  }
  else if (key == '*') {
    currentMode = MODE_JADWAL_MENU; 
    showJadwalMenu();
  }
}