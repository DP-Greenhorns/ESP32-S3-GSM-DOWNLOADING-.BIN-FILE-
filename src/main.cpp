
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>

// ============================================================================
// Forward Declarations
// These tell main.cpp that these functions are defined in gsm.cpp
// ============================================================================
bool gsm_setup();
void startDownload(); 

// WebServer instance (optional - currently not used)
WebServer server(80);

// Network Credentials for SoftAP (optional, currently commented out)
const char *ssid = "ESP32-S3-AccessPoint";
const char *password = "12345678";

// --- Handler Functions ---
void handleRoot() {
  server.send(200, "text/plain", "Welcome to ESP32-S3 SoftAP Server!");
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not Found");
}

void setup() {
  // Start Serial Monitor
  Serial.begin(115200);
  delay(2000); // Give serial time to initialize

  Serial.println("\n\n");
  Serial.println("==============================================");
  Serial.println("ESP32-S3 FIRMWARE DOWNLOADER (SPIFFS)");
  Serial.println("==============================================");

  // Mount SPIFFS
  // 'true' asks it to format if the file system is corrupted
  if (!SPIFFS.begin(true)) {
    Serial.println("✗ SPIFFS mount failed");
    Serial.println("System halted.");
    while(1) delay(1000);
  } else {
    Serial.println("✓ SPIFFS mounted successfully");
  }

  // Optional: Setup SoftAP (currently commented out)
  // WiFi.softAP(ssid, password);
  // IPAddress IP = WiFi.softAPIP();
  // Serial.print("AP IP: ");
  // Serial.println(IP);
  // server.on("/", handleRoot);
  // server.onNotFound(handleNotFound);
  // server.begin();

  // Initialize and run GSM download with checksum
  if (gsm_setup()) {
    Serial.println("\n✓ GSM Setup Successful - Starting Download...\n");
    
    // ============================================================
    // TRIGGER THE DOWNLOAD
    // This calls the function inside gsm.cpp
    // ============================================================
    startDownload(); 

    Serial.println("\n==============================================");
    Serial.println("DOWNLOAD PROCESS COMPLETED");
    Serial.println("==============================================\n");
  } else {
    Serial.println("\n✗ GSM Setup Failed - Cannot download\n");
  }
}

void loop() {
  // Optional: Handle web server requests if enabled
  // server.handleClient();
  
  // Nothing to do after download completes
  delay(10000);
}