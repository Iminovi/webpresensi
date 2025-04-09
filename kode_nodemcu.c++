#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define BUZZER_PIN D7
SoftwareSerial mySerial(D1, D2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RTC_DS3231 rtc;

const char* ssid = "NAMA_WIFI";
const char* password = "PASSWORD_WIFI";
#define FIREBASE_HOST "https://absensi-device1-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH "YOUR_API_KEY_DEVICE1"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

#define EEPROM_SIZE 512
int writeAddr = 0;
bool checkedToday = false;

struct Shift {
  int masukStart;
  int masukEnd;
  int pulangStart;
  int pulangEnd;
};
Shift shiftConfig[3]; // 0: Pagi, 1: Siang, 2: Malam

void setup() {
  Serial.begin(115200);
  mySerial.begin(57600);
  finger.begin(57600);

  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(D5, D6);
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

  loadShiftConfig();
  syncEEPROMData();
}

void loop() {
  int id = getFingerprintID();
  if (id >= 0) checkTimeAndSend(id);

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

    Firebase.RTDB.getJSON(&fbdo, "/fingerprint/delete");
    if (fbdo.dataAvailable()) {
      FirebaseJson json = fbdo.jsonObject();
      String status;
      json.get(status, "status");
      if (status == "pending") {
        int fingerId;
        json.get(fingerId, "id");
        deleteFingerprintOnDevice(fingerId);
      }
    }
  }

  DateTime now = rtc.now();
  if (now.hour() == 17 && now.minute() == 0 && !checkedToday) {
    checkAbsensiHarian();
    checkedToday = true;
  }
  if (now.hour() == 0 && now.minute() == 0) checkedToday = false;

  delay(1000);
}

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

void checkTimeAndSend(int id) {
  DateTime now = rtc.now();
  String nama = getKaryawanName(id);
  if (nama == "") nama = "Karyawan" + String(id);
  String shift = getKaryawanShift(id);
  String status;

  int totalMinutes = now.hour() * 60 + now.minute();
  Shift currentShift;
  if (shift == "Pagi") currentShift = shiftConfig[0];
  else if (shift == "Siang") currentShift = shiftConfig[1];
  else if (shift == "Malam") currentShift = shiftConfig[2];
  else {
    buzzFail();
    displayMessage("Shift tidak diketahui!");
    return;
  }

  if (totalMinutes >= currentShift.masukStart && totalMinutes <= currentShift.masukEnd) {
    status = "Masuk";
  } else if (totalMinutes >= currentShift.pulangStart && totalMinutes <= currentShift.pulangEnd) {
    status = "Pulang";
  } else if (shift == "Malam" && totalMinutes >= 0 && totalMinutes <= currentShift.pulangEnd) {
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

String getKaryawanName(int id) {
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    Firebase.RTDB.getString(&fbdo, "/karyawan/" + String(id));
    if (fbdo.dataAvailable()) return fbdo.stringData();
  }
  return "";
}

String getKaryawanShift(int id) {
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    Firebase.RTDB.getString(&fbdo, "/shift/" + String(id));
    if (fbdo.dataAvailable()) return fbdo.stringData();
  }
  return "";
}

void loadShiftConfig() {
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    String shifts[] = {"Pagi", "Siang", "Malam"};
    for (int i = 0; i < 3; i++) {
      Firebase.RTDB.getJSON(&fbdo, "/shift_config/" + shifts[i]);
      if (fbdo.dataAvailable()) {
        FirebaseJson json = fbdo.jsonObject();
        json.get(shiftConfig[i].masukStart, "masukStart");
        json.get(shiftConfig[i].masukEnd, "masukEnd");
        json.get(shiftConfig[i].pulangStart, "pulangStart");
        json.get(shiftConfig[i].pulangEnd, "pulangEnd");
      }
    }
  }
}

void checkAbsensiHarian() {
  DateTime now = rtc.now();
  String tanggalHariIni = String(now.day()) + "-" + String(now.month()) + "-" + String(now.year());

  FirebaseJson karyawanJson;
  Firebase.RTDB.getJSON(&fbdo, "/karyawan");
  if (!fbdo.dataAvailable()) return;
  karyawanJson = fbdo.jsonObject();

  FirebaseJson izinJson;
  String izinPath = "/izin/" + tanggalHariIni;
  Firebase.RTDB.getJSON(&fbdo, izinPath);
  bool hasIzin = fbdo.dataAvailable();
  if (hasIzin) izinJson = fbdo.jsonObject();

  FirebaseJson absensiJson;
  Firebase.RTDB.getJSON(&fbdo, "/absensi");
  String absensiHariIni[1000] = {""}; // Buffer sementara, sesuaikan kapasitas
  int absensiCount = 0;
  if (fbdo.dataAvailable()) {
    absensiJson = fbdo.jsonObject();
    FirebaseJsonData result;
    for (FirebaseJson::Iterator it = absensiJson.iteratorBegin(); it != absensiJson.iteratorEnd(); ++it) {
      String key = it.key();
      absensiJson.get(result, key);
      FirebaseJson absensi = result.jsonObject();
      String nama, tanggal;
      absensi.get(nama, "nama");
      absensi.get(tanggal, "tanggal");
      if (tanggal == tanggalHariIni && absensiCount < 1000) {
        absensiHariIni[absensiCount++] = nama;
      }
    }
  }

  FirebaseJsonData karyawanData;
  for (FirebaseJson::Iterator it = karyawanJson.iteratorBegin(); it != karyawanJson.iteratorEnd(); ++it) {
    String id = it.key();
    karyawanJson.get(karyawanData, id);
    String nama = karyawanData.stringValue;
    bool sudahAbsen = false;
    for (int i = 0; i < absensiCount; i++) {
      if (absensiHariIni[i] == nama) {
        sudahAbsen = true;
        break;
      }
    }
    bool izin = false;
    if (hasIzin) izinJson.get(izin, nama);
    if (nama != "" && !sudahAbsen && !izin) {
      String data = nama + ",Tidak Masuk,-," + tanggalHariIni;
      if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        sendToFirebase(data);
      } else {
        saveToEEPROM(data);
      }
    }
  }
  displayMessage("Cek Tidak Masuk");
}

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
    Firebase.RTDB.setString(&fbdo, "/fingerprint/register/status", "success");
    buzzSuccess();
    displayMessage("Daftar Sukses");
  } else {
    Firebase.RTDB.setString(&fbdo, "/fingerprint/register/status", "failed");
    displayMessage("Gagal Daftar");
  }
}

void deleteFingerprintOnDevice(int id) {
  uint8_t p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    Firebase.RTDB.remove(&fbdo, "/karyawan/" + String(id));
    Firebase.RTDB.remove(&fbdo, "/shift/" + String(id));
    Firebase.RTDB.setString(&fbdo, "/fingerprint/delete/status", "success");
    buzzSuccess();
    displayMessage("Hapus Sukses");
  } else {
    Firebase.RTDB.setString(&fbdo, "/fingerprint/delete/status", "failed");
    displayMessage("Hapus Gagal");
  }
}

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
