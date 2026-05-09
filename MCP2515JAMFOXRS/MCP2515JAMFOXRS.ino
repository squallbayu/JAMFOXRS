#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>           
#include "fox_config.h"
#include "fox_display.h"
#include "fox_canbus.h"
#include "fox_rtc.h"
#include "fox_page.h"
#include "fox_task.h"
#include "fox_serial.h"
#include "fox_ble.h"

// =============================================
// GLOBAL VARIABLES
// =============================================
bool systemReady = false;
unsigned long systemStartTime = 0;

extern bool displayInitialized;
extern bool displayReady;
extern int currentPage;
extern uint8_t currentSpecialMode;
extern bool setupMode;

// =============================================
// SETUP FUNCTION - MCP2515 VERSION
// =============================================
void setup() {
    Serial.begin(115200);
    delay(100);
    
    systemStartTime = millis();
    printSystemStartup();
    
    // Initialize I2C for OLED and RTC
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);
    #ifdef ESP32
    Wire.setTimeOut(500);
    #endif
    delay(100);
    
    // Initialize SPI for MCP2515
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, MCP_CS_PIN);
    delay(100);
    
    // Initialize display FIRST
    initDisplay();
    
    // Initialize RTC
    if (initRTC()) {
        serialPrintflnAlways("[RTC] DS3231 detected");
    } else {
        serialPrintflnAlways("[RTC] DS3231 not found, using compile time");
        setRTCFromCompileTime();
    }
    
    #ifdef ESP32
    initFreeRTOS();
    
    // Initialize CAN (MCP2515)
    if (!initCAN()) {
        serialPrintflnAlways("[SYSTEM] WARNING: MCP2515 CAN initialization failed");
        serialPrintflnAlways("[SYSTEM] Check wiring: SCK=4, MISO=5, MOSI=6, CS=7");
    } else {
        serialPrintflnAlways("[SYSTEM] MCP2515 CAN initialized successfully");
    }
    
    // Initialize display task AFTER display is ready
    initDisplayTask();
    
    createTasks();  // Create tasks LAST
    #endif
    
    initButton();
    
    currentPage = getFirstEnabledPage();
    
    systemReady = true;
    
    // Update display once
    if(displayReady) {
        delay(100); // Beri waktu display task start
        safeDisplayUpdate(currentPage);
    }
    
    serialPrintflnAlways("\n[SYSTEM] Setup Complete");
    serialPrintflnAlways("[SYSTEM] CAN: MCP2515 @ %d kbps", CAN_BAUDRATE / 1000);
    serialPrintflnAlways("[SYSTEM] BLE: 5-second hold to activate/deactivate");
    serialPrintflnAlways("[SYSTEM] Initial page: %d", currentPage);
}

// =============================================
// MODIFIED LOOP - LEBIH RINGAN
// =============================================
void loop() {
#ifdef ESP32
    unsigned long now = millis();
    
    // 1. SYSTEM HEALTH MONITOR (kurangi frekuensi)
    static unsigned long lastHealthUpdate = now;
    if(now - lastHealthUpdate > 3000) {  // 3 detik instead of 1 detik
        updateSystemHealth();
        lastHealthUpdate = now;
    }
    
    // 2. SERIAL COMMANDS
    processSerialCommands();
    
    // 3. BUTTON HANDLING
    if(checkButtonPress()) {
        handleButtonPress();
    }
    
    // 4. BLE PROCESSING
    #ifdef ESP32
    processBLE();
    #endif
    
    // 5. WATCHDOG
    static unsigned long lastLoopHeartbeat = now;
    if(now - lastLoopHeartbeat > 10000) {  // Reset setelah 10 detik tanpa heartbeat
        lastLoopHeartbeat = now;
    }
    
    delay(10);  // Beri waktu untuk task lain
    
#else
    // NON-ESP32 VERSION
    processSerialCommands();
    
    if(checkButtonPress()) {
        handleButtonPress();
    }
    
    static unsigned long lastDisplayUpdate = 0;
    if(millis() - lastDisplayUpdate > 5000) {
        safeDisplayUpdate(currentPage);
        lastDisplayUpdate = millis();
    }
    
    delay(100);
#endif
}
