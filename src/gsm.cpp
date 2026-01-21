
#include <Arduino.h>
#include <SPIFFS.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PIN_CELL_TX      6
#define PIN_CELL_RX      5
#define PIN_CELL_PWRKEY  4
#define PIN_CELL_RST     7
#define UART_CELLULAR    2
#define BAUD_CELLULAR    115200

// URL with dynamic query param support
const char* URL_BASE = "http://digitalpetro.s3.ap-south-1.amazonaws.com/BPCL/New+PCB+Bootcode/bootcode.bin";
const char* FILE_PATH = "/bootcode.bin";
#define CHUNK_SIZE 4096  // Increased chunk size for better throughput

HardwareSerial CellUART(UART_CELLULAR);

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
bool gsm_setup();
bool system_init(); // New function to handle SPIFFS init
void startDownload();
void downloadAndVerify(const String& url);
void calculateStorageChecksum();
bool sendAT(String cmd, const char* expected, uint32_t timeout);
String waitForResponse(uint32_t timeout);
void powerCycleModem();
bool connectNetwork();
void printProgress(size_t current, size_t total);

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

// Call this function first in your main setup()
bool system_init() {
    Serial.begin(115200);
    
    // 1. CRITICAL FIX: Initialize SPIFFS
    // 'true' allows it to format the drive if it is corrupt or empty
    if (!SPIFFS.begin(true)) {
        Serial.println("CRITICAL ERROR: SPIFFS Mount Failed");
        return false;
    }
    Serial.println("✓ SPIFFS Mounted Successfully");
    return true;
}

bool gsm_setup() {
    // 2. CRITICAL FIX: Increase RX Buffer to prevent overflow during Flash writes
    CellUART.setRxBufferSize(CHUNK_SIZE + 512); 
    CellUART.begin(BAUD_CELLULAR, SERIAL_8N1, PIN_CELL_RX, PIN_CELL_TX);
    
    Serial.println("Connecting to GSM...");
    
    // Initial Modem Power Up Sequence
    powerCycleModem();

    for(int i=0; i<3; i++) {
        if(connectNetwork()) return true;
        Serial.println("Retrying GSM...");
        powerCycleModem();
    }
    return false;
}

void startDownload() {
    // Add cache busting timestamp
    String finalURL = String(URL_BASE) + "?t=" + String(millis());
    downloadAndVerify(finalURL);
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void downloadAndVerify(const String& downloadUrl) {
    Serial.println("\n----------------------------------------------");
    Serial.println("STARTING DOWNLOAD (Rectified Version)");
    Serial.println("----------------------------------------------");

    // 1. Prepare Modem
    sendAT("ATE0", "OK", 1000);
    sendAT("AT+QHTTPSTOP", "OK", 1000);
    sendAT("AT+QHTTPCFG=\"responseheader\",0", "OK", 1000);

    // 2. Set URL
    CellUART.println("AT+QHTTPURL=" + String(downloadUrl.length()) + ",80");
    if (waitForResponse(5000).indexOf("CONNECT") == -1) {
        Serial.println("✗ Error: URL CONNECT failed");
        return;
    }
    CellUART.print(downloadUrl);
    waitForResponse(5000);

    // 3. HTTP GET (Download from Server to Modem)
    // We give the modem 60s to connect to server and headers
    CellUART.println("AT+QHTTPGET=80"); 
    
    long fileSize = -1;
    unsigned long startWait = millis();
    bool getSizeSuccess = false;

    // Wait for +QHTTPGET: 0,200,<size>
    while (millis() - startWait < 80000) { 
        if (CellUART.available()) {
            String line = CellUART.readStringUntil('\n');
            if (line.startsWith("+QHTTPGET: 0,200")) {
                fileSize = line.substring(line.lastIndexOf(',') + 1).toInt();
                Serial.printf("✓ Target File Size: %ld bytes\n", fileSize);
                getSizeSuccess = true;
                break;
            }
            if (line.startsWith("+QHTTPGET: ") && !line.startsWith("+QHTTPGET: 0,200")) {
                 Serial.println("✗ HTTP GET Error: " + line);
                 return;
            }
        }
    }

    if (!getSizeSuccess || fileSize <= 0) {
        Serial.println("✗ Failed to get file size. Check URL or Network.");
        return;
    }

    // 4. Clean SPIFFS before writing
    if (SPIFFS.exists(FILE_PATH)) SPIFFS.remove(FILE_PATH);
    
    // NOTE: SPIFFS.begin() must have been called in system_init() for this to work
    File file = SPIFFS.open(FILE_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("✗ SPIFFS Write Error - Did you call SPIFFS.begin()?"); 
        return;
    }

    // 5. Read Data (Modem to ESP32)
    // Timeout increased to 300 seconds (5 mins) for 1MB file
    CellUART.println("AT+QHTTPREAD=300");

    // Wait for CONNECT
    unsigned long connectTime = millis();
    bool connected = false;
    while (millis() - connectTime < 10000) {
        if (CellUART.available()) {
            String line = CellUART.readStringUntil('\n');
            if (line.indexOf("CONNECT") != -1) {
                connected = true;
                break;
            }
        }
    }

    if (!connected) {
        Serial.println("✗ Modem did not start data stream (No CONNECT)");
        file.close();
        return;
    }

    // 6. Binary Read Loop
    long bytesDownloaded = 0;
    
    // FIX: Check if malloc succeeded
    uint8_t* buffer = (uint8_t*)malloc(CHUNK_SIZE);
    if(buffer == NULL) {
        Serial.println("✗ Memory Allocation Failed");
        file.close();
        return;
    }

    unsigned long lastAct = millis();
    const unsigned long INACTIVITY_TIMEOUT = 60000; 

    while (bytesDownloaded < fileSize) {
        // Feed watchdog
        yield(); 

        if (CellUART.available()) {
            size_t toRead = min((size_t)CHUNK_SIZE, (size_t)CellUART.available());
            
            // Cap read to remaining file size (prevents reading trailing OK)
            if ((bytesDownloaded + toRead) > fileSize) {
                toRead = fileSize - bytesDownloaded;
            }

            int len = CellUART.readBytes(buffer, toRead);
            if (len > 0) {
                file.write(buffer, len);
                bytesDownloaded += len;
                lastAct = millis();
                if (bytesDownloaded % 51200 == 0) printProgress(bytesDownloaded, fileSize);
            }
        }
        
        if (millis() - lastAct > INACTIVITY_TIMEOUT) {
            Serial.println("\n✗ ERROR: Data stream timed out (Host inactive)");
            break;
        }
    }

    file.close();
    free(buffer);

    if (bytesDownloaded == fileSize) {
        Serial.printf("\n✓ Download Success: %ld / %ld bytes\n", bytesDownloaded, fileSize);
        calculateStorageChecksum();
    } else {
        Serial.printf("\n✗ Download INCOMPLETE: %ld / %ld bytes\n", bytesDownloaded, fileSize);
        Serial.println("  (Deleting incomplete file...)");
        SPIFFS.remove(FILE_PATH); 
    }
}

void calculateStorageChecksum() {
    Serial.println("\n--- VERIFYING SPIFFS FILE ---");
    File file = SPIFFS.open(FILE_PATH, FILE_READ);
    if (!file) {
        Serial.println("Failed to open file for verification");
        return;
    }

    mbedtls_md5_context md5_ctx;
    mbedtls_md5_init(&md5_ctx);
    mbedtls_md5_starts_ret(&md5_ctx);

    uint8_t* vBuf = (uint8_t*)malloc(CHUNK_SIZE);
    if (!vBuf) {
        file.close();
        Serial.println("Memory error in verification");
        return;
    }

    while (file.available()) {
        int len = file.read(vBuf, CHUNK_SIZE);
        mbedtls_md5_update_ret(&md5_ctx, vBuf, len);
    }

    uint8_t md5Res[16];
    mbedtls_md5_finish_ret(&md5_ctx, md5Res);
    
    file.close();
    free(vBuf);
    mbedtls_md5_free(&md5_ctx);

    Serial.print("MD5: ");
    for (int i = 0; i < 16; i++) Serial.printf("%02x", md5Res[i]);
    Serial.println("\n----------------------------------------------");
}

bool connectNetwork() {
    sendAT("ATE0", "OK", 1000);
    if (!sendAT("AT+CPIN?", "READY", 2000)) return false;
    sendAT("AT+QIDEACT=1", "OK", 5000);
    // Be sure APN is correct
    if (!sendAT("AT+QICSGP=1,1,\"airtelgprs.com\",\"\",\"\",1", "OK", 2000)) return false;
    if (!sendAT("AT+QIACT=1", "OK", 10000)) return false;
    return true;
}

void powerCycleModem() {
    Serial.println("Power cycling modem...");
    pinMode(PIN_CELL_RST, OUTPUT);
    pinMode(PIN_CELL_PWRKEY, OUTPUT);

    // Reset Sequence
    digitalWrite(PIN_CELL_RST, HIGH);
    delay(200);
    digitalWrite(PIN_CELL_RST, LOW);
    delay(3000);

    // Power Key Sequence
    digitalWrite(PIN_CELL_PWRKEY, HIGH);
    delay(1000);
    digitalWrite(PIN_CELL_PWRKEY, LOW);
    delay(5000);
}

bool sendAT(String cmd, const char* expected, uint32_t timeout) {
    if (cmd.length()) CellUART.println(cmd);
    uint32_t start = millis();
    String resp = "";
    while (millis() - start < timeout) {
        if (CellUART.available()) {
            resp += char(CellUART.read());
            if (resp.indexOf(expected) != -1) return true;
        }
    }
    return false;
}

String waitForResponse(uint32_t timeout) {
    String resp = "";
    uint32_t start = millis();
    while (millis() - start < timeout) {
        while (CellUART.available()) resp += char(CellUART.read());
    }
    return resp;
}

void printProgress(size_t current, size_t total) {
    Serial.printf("Downloading: %d%% (%ld B)\n", (int)((current * 100) / total), current);
}