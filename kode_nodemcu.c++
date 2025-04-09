#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// Definisi pin dan konstanta
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define BUZZER_PIN D7
SoftwareSerial mySerial(D1, D2); // D1 (TX), D2 (RX) untuk sensor sidik jari
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RTC_DS3231 rtc;

const char* ssid = "NAMA_WIFI";
const char* password = "PASSWORD_WIFI";
#define FIREBASE_HOST "https://absensi-device1-default-rtdb.asia-southeast1.firebasedatabase.app/" // Ganti per device
#define FIREBASE_AUTH "YOUR_API_KEY_DEVICE1" // Ganti per device

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// EEPROM dan variabel global
#define EEPROM_SIZE 512
int writeAddr = 0;
String karyawanList[33]; // Array untuk nama karyawan (ID 1-32)
bool checkedToday = false; // Flag untuk cek "Tidak Masuk" sekali sehari

void setup() {
  Serial.begin(115200);
  mySerial.begin(57600);
  finger.begin(57600);

  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(D5, D6); // D5 (SCL), D6 (SDA) untuk I2C
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  if (!rtc.begin()) { for(;;); }
  EEPROM.begin(EEPROM_SIZE);

  welcomeAnimation();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Terhubung ke Wi-Fi");

  config.api_key = FIREBASE_AUTH;
  config.database_url = FIREBASE_HOST;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (finger.verifyPassword()) {
    Serial.println("Sensor sidik jari ditemukan!");
  } else {
    Serial.println("Sensor sidik jari gagal!");
    for(;;);
  }

  loadKaryawan(); // Muat daftar karyawan dari Firebase
  syncEEPROMData(); // Sinkron data offline saat startup
}

void loop() {
  int id = getFingerprintID();
  if (id >= 0) checkTimeAndSend(id);

  // Cek pendaftaran sidik jari dari web
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    Firebase.RTDB.getJSON(&fbdo, "/fingerprint/register");
    if (fbdo.dataAvailable()) {
      FirebaseJson json = fbdo.jsonObject();
      String status;
      json.get(status, "status");
      if (status == "pending") {
        int fingerId;
        String fingerName;
        json.get(fingerId, "id");
        json.get(fingerName, "name");
        registerFingerprintOnDevice(fingerId, fingerName);
      }
    }
  }

  // Otomatisasi "Tidak Masuk" jam 17:00
  DateTime now = rtc.now();
  if (now.hour() == 17 && now.minute() == 0 && !checkedToday) {
    checkAbsensiHarian();
    checkedToday = true;
  }
  if (now.hour() == 0 && now.minute() == 0) checkedToday = false; // Reset flag tengah malam

  delay(1000);
}

// Fungsi untuk baca sidik jari
int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  else {
    buzzFail();
    displayMessage("Gagal Absen");
    return -1;
  }
}

// Cek waktu absensi dan kirim data
void checkTimeAndSend(int id) {
  DateTime now = rtc.now();
  String nama = (id >= 1 && id <= 32) ? karyawanList[id] : "Unknown";
  if (nama == "Unknown" || nama == "") nama = "Karyawan" + String(id);
  String status;

  int hour = now.hour();
  int minute = now.minute();
  int totalMinutes = hour * 60 + minute;

  int masukStart = 16 * 60 + 30; // 16:30
  int masukEnd = 17 * 60;        // 17:00
  int pulangStart = 20 * 60 + 10; // 20:10
  int pulangEnd = 20 * 60 + 40;   // 20:40

  if (totalMinutes >= masukStart && totalMinutes <= masukEnd) {
    status = "Masuk";
  } else if (totalMinutes >= pulangStart && totalMinutes <= pulangEnd) {
    status = "Pulang";
  } else {
    buzzFail();
    displayMessage("Absen di luar jam!");
    return;
  }

  String waktu = String(now.hour()) + ":" + (now.minute() < 10 ? "0" : "") + String(now.minute());
  String tanggal = String(now.day()) + "-" + String(now.month()) + "-" + String(now.year());
  String data = nama + "," + status + "," + waktu + "," + tanggal;

  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    sendToFirebase(data);
  } else {
    saveToEEPROM(data);
    displayMessage("Disimpan lokal");
  }
}

// Kirim data ke Firebase
void sendToFirebase(String data) {
  String path = "/absensi/" + String(millis());
  FirebaseJson json;
  String parts[4];
  splitString(data, ',', parts, 4);
  json.set("nama", parts[0]);
  json.set("status", parts[1]);
  json.set("waktu", parts[2]);
  json.set("tanggal", parts[3]);

  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    buzzSuccess();
    displayMessage("Absen Berhasil");
  } else {
    saveToEEPROM(data);
    displayMessage("Disimpan lokal");
  }
}

// Simpan ke EEPROM saat offline
void saveToEEPROM(String data) {
  if (writeAddr + data.length() + 1 >= EEPROM_SIZE) {
    displayMessage("EEPROM penuh!");
    return;
  }
  for (int i = 0; i < data.length(); i++) {
    EEPROM.write(writeAddr + i, data[i]);
  }
  EEPROM.write(writeAddr + data.length(), '\n');
  writeAddr += data.length() + 1;
  EEPROM.commit();
}

// Sinkron data EEPROM saat online
void syncEEPROMData() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  String buffer = "";
  for (int i = 0; i < writeAddr; i++) {
    char c = EEPROM.read(i);
    if (c == '\n') {
      sendToFirebase(buffer);
      buffer = "";
    } else {
      buffer += c;
    }
  }
  if (buffer == "") {
    for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
    writeAddr = 0;
    EEPROM.commit();
    displayMessage("Sync selesai");
  }
}

// Muat daftar karyawan dari Firebase
void loadKaryawan() {
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    Firebase.RTDB.getJSON(&fbdo, "/karyawan");
    if (fbdo.dataAvailable()) {
      FirebaseJson json = fbdo.jsonObject();
      for (int i = 1; i <= 32; i++) {
        String key = String(i);
        json.get(karyawanList[i], key);
      }
    }
  }
}

// Cek absensi harian untuk "Tidak Masuk"
void checkAbsensiHarian() {
  DateTime now = rtc.now();
  String tanggalHariIni = String(now.day()) + "-" + String(now.month()) + "-" + String(now.year());

  // Ambil absensi hari ini dari Firebase
  Firebase.RTDB.getJSON(&fbdo, "/absensi");
  String absensiHariIni[33] = {""};
  if (fbdo.dataAvailable()) {
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData result;
    for (FirebaseJson::Iterator it = json.iteratorBegin(); it != json.iteratorEnd(); ++it) {
      String key = it.key();
      json.get(result, key);
      FirebaseJson absensi = result.jsonObject();
      String nama, tanggal;
      absensi.get(nama, "nama");
      absensi.get(tanggal, "tanggal");
      if (tanggal == tanggalHariIni) {
        for (int i = 1; i <= 32; i++) {
          if (karyawanList[i] == nama) absensiHariIni[i] = nama;
        }
      }
    }
  }

  // Tambah "Tidak Masuk" untuk karyawan yang belum absen
  for (int i = 1; i <= 32; i++) {
    if (karyawanList[i] != "" && absensiHariIni[i] == "") {
      String data = karyawanList[i] + ",Tidak Masuk,-," + tanggalHariIni;
      if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        sendToFirebase(data);
      } else {
        saveToEEPROM(data);
      }
    }
  }
  displayMessage("Cek Tidak Masuk");
}

// Pendaftaran sidik jari dari web
void registerFingerprintOnDevice(int id, String name) {
  displayMessage("Scan sidik jari");
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return;
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) return;
  displayMessage("Lepas jari");
  delay(2000);
  p = finger.getImage();
  if (p != FINGERPRINT_OK) return;
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) return;
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Firebase.RTDB.setString(&fbdo, "/fingerprint/register/status", "failed");
    displayMessage("Gagal Daftar");
    return;
  }
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    karyawanList[id] = name; // Update lokal
    Firebase.RTDB.setString(&fbdo, "/fingerprint/register/status", "success");
    buzzSuccess();
    displayMessage("Daftar Sukses");
  } else {
    Firebase.RTDB.setString(&fbdo, "/fingerprint/register/status", "failed");
    displayMessage("Gagal Daftar");
  }
}

// Fungsi pendukung
void splitString(String str, char delimiter, String* output, int maxParts) {
  int part = 0, start = 0;
  for (int i = 0; i < str.length() && part < maxParts; i++) {
    if (str[i] == delimiter) {
      output[part++] = str.substring(start, i);
      start = i + 1;
    }
  }
  output[part] = str.substring(start);
}

void displayMessage(String msg) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.println(msg);
  display.display();
}

void welcomeAnimation() {
  displayMessage("Selamat Datang");
  delay(2000);
}

void buzzSuccess() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
}

void buzzFail() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);
}
