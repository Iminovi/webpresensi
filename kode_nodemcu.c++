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
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BUZZER_PIN D7
RTC_DS3231 rtc;
SoftwareSerial mySerial(D1, D2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// Ganti per device
const char* ssid = "NAMA_WIFI";
const char* password = "PASSWORD_WIFI";
#define FIREBASE_HOST "https://absensi-device1-default-rtdb.asia-southeast1.firebasedatabase.app/" // Unik per device
#define FIREBASE_AUTH "YOUR_API_KEY_DEVICE1" // Unik per device

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// EEPROM setup
#define EEPROM_SIZE 512
int writeAddr = 0; // Posisi tulis di EEPROM

void setup() {
  Serial.begin(115200);
  mySerial.begin(57600);
  finger.begin(57600);

  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(D5, D6);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  if (!rtc.begin()) { for(;;); }

  EEPROM.begin(EEPROM_SIZE); // Inisialisasi EEPROM
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
    for(;;);
  }

  // Sync data dari EEPROM saat pertama online
  syncEEPROMData();
}

void loop() {
  int id = getFingerprintID();
  if (id >= 0) {
    checkTimeAndSend(id);
  }

  // Cek perintah pendaftaran (opsional)
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
  String nama = "Karyawan" + String(id); // Ganti dengan mapping nama
  String status;

  int hour = now.hour();
  int minute = now.minute();
  int totalMinutes = hour * 60 + minute;

  int masukStart = 16 * 60 + 30;
  int masukEnd = 17 * 60;
  int pulangStart = 20 * 60 + 10;
  int pulangEnd = 20 * 60 + 40;

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
    return; // Lindungi overflow
  }

  for (int i = 0; i < data.length(); i++) {
    EEPROM.write(writeAddr + i, data[i]);
  }
  EEPROM.write(writeAddr + data.length(), '\n'); // Penanda akhir entri
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

  // Reset EEPROM setelah sync
  if (buffer == "") {
    for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
    writeAddr = 0;
    EEPROM.commit();
    displayMessage("Sync selesai");
  }
}

void splitString(String str, char delimiter, String* output, int maxParts) {
  int part = 0;
  int start = 0;
  for (int i = 0; i < str.length() && part < maxParts; i++) {
    if (str[i] == delimiter) {
      output[part++] = str.substring(start, i);
      start = i + 1;
    }
  }
  output[part] = str.substring(start);
}

void registerFingerprintOnDevice(int id, String name) {
  displayMessage("Letakkan jari...");
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    updateRegisterStatus("failed");
    displayMessage("Gagal deteksi jari");
    return;
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    updateRegisterStatus("failed");
    displayMessage("Gagal konversi");
    return;
  }

  displayMessage("Lepaskan jari...");
  delay(2000);
  p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    updateRegisterStatus("failed");
    displayMessage("Gagal deteksi lagi");
    return;
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    updateRegisterStatus("failed");
    displayMessage("Gagal konversi lagi");
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    updateRegisterStatus("failed");
    displayMessage("Gagal buat model");
    return;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    updateRegisterStatus("success");
    displayMessage("Pendaftaran sukses");
    buzzSuccess();
  } else {
    updateRegisterStatus("failed");
    displayMessage("Gagal simpan");
    buzzFail();
  }
}

void updateRegisterStatus(String status) {
  FirebaseJson json;
  json.set("status", status);
  Firebase.RTDB.setJSON(&fbdo, "/fingerprint/register", &json);
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
