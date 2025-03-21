#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BUZZER_PIN D7
RTC_DS3231 rtc;
SoftwareSerial mySerial(D1, D2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

const char* ssid = "NAMA_WIFI";//
const char* password = "PASSWORD_WIFI";
#define FIREBASE_HOST "https://absensi-41dac-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH "AIzaSyDZWUUArpxE3w5C1Nywbf990Q6vYjjsrJg"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

void setup() {
  Serial.begin(115200);
  mySerial.begin(57600);
  finger.begin(57600);

  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(D5, D6);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  if (!rtc.begin()) { for(;;); }

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
}

void loop() {
  // Mode absensi normal
  int id = getFingerprintID();
  if (id >= 0) {
    checkTimeAndSend(id);
  }

  // Cek perintah pendaftaran dari Firebase
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
  String nama = "Karyawan" + String(id); // Ganti dengan mapping nama jika ada
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
  sendToFirebase(nama, status, waktu);
}

void sendToFirebase(String nama, String status, String waktu) {
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    String path = "/absensi/" + String(millis());
    FirebaseJson json;
    json.set("nama", nama);
    json.set("status", status);
    json.set("waktu", waktu);
    json.set("tanggal", String(rtc.now().day()) + "-" + String(rtc.now().month()) + "-" + String(rtc.now().year()));

    if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
      buzzSuccess();
      displayMessage("Absen Berhasil");
    } else {
      buzzFail();
      displayMessage("Gagal Kirim");
    }
  }
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
