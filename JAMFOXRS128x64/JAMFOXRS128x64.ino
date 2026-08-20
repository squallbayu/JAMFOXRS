#include <Arduino.h>
#include <Wire.h>
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
// SETUP FUNCTION
// =============================================
void setup() {
    Serial.begin(115200);
    delay(100);
    
    systemStartTime = millis();
    printSystemStartup();
    
    // =============================================
    // TAMBAHAN UNTUK DISPLAY 0.96 INCH
    // Beri waktu display stabil sebelum inisialisasi
    // =============================================
    delay(500);
    
    // Inisialisasi I2C dengan kecepatan dari config
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(I2C_CLOCK_SPEED);  // Gunakan I2C_CLOCK_SPEED dari config (100kHz)
    #ifdef ESP32
    Wire.setTimeOut(500);
    #endif
    delay(100);
    
    // Inisialisasi display
    initDisplay();  // Display initialization FIRST
    
    // Inisialisasi RTC
    if (initRTC()) {
        serialPrintflnAlways("[SYSTEM] RTC initialized successfully");
    } else {
        serialPrintflnAlways("[SYSTEM] RTC not found, using compile time");
        setRTCFromCompileTime();
    }
    
    #ifdef ESP32
    // Inisialisasi FreeRTOS
    initFreeRTOS();
    
    // Inisialisasi CAN
    if (!initCAN()) {
        serialPrintflnAlways("[SYSTEM] WARNING: CAN initialization failed");
    }
    
    // Initialize display task AFTER display is ready
    initDisplayTask();
    
    // Create tasks LAST
    createTasks();
    #endif
    
    // Inisialisasi button
    initButton();
    
    // Set current page ke page pertama yang enabled
    currentPage = getFirstEnabledPage();
    
    systemReady = true;
    
    // Update display once
    if(displayReady) {
        delay(100); // Beri waktu display task start
        safeDisplayUpdate(currentPage);
    }
    
    serialPrintflnAlways("\n[SYSTEM] Setup Complete");
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
