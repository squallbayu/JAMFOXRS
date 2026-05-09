#include "fox_canbus.h"
#include "fox_config.h"
#include "fox_vehicle.h"
#include "fox_serial.h"
#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#endif

// =============================================
// MCP2515 CAN OBJECT
// =============================================
MCP_CAN CAN0(MCP_CS_PIN);

// =============================================
// EXTERN VARIABLES
// =============================================
extern VehicleData vehicle;
#ifdef ESP32
extern SemaphoreHandle_t dataMutex;
#endif

// =============================================
// GLOBAL REAL-TIME ATOMIC VARIABLES
// =============================================
#ifdef ESP32
// ATOMIC variables for real-time voltage/current
std::atomic<float> realtimeVoltage{0.0f};
std::atomic<float> realtimeCurrent{0.0f};
std::atomic<uint32_t> realtimeUpdateTime{0};

// Charger detection atomic variables
std::atomic<bool> chargerConnected{false};
std::atomic<bool> oriChargerDetected{false};
std::atomic<uint32_t> lastChargerMsgTime{0};
std::atomic<uint32_t> lastOriChargerMsgTime{0};

// Charging mode flag
std::atomic<bool> isChargingMode{false};

// System health monitor
std::atomic<uint32_t> lastSuccessfulLoop{0};
std::atomic<uint32_t> systemErrorCount{0};

// CAN statistics
std::atomic<uint32_t> canMessageCount{0};
std::atomic<uint32_t> canMessagesPerSecond{0};

// Charger message counters
std::atomic<uint32_t> chargerMessageCount{0};
std::atomic<uint32_t> oriChargerMessageCount{0};
#endif

// =============================================
// LOCAL TIMING FOR STATISTICS
// =============================================
static uint32_t lastStatsTime = 0;
static uint32_t localMsgCount = 0;

// =============================================
// CAN INITIALIZATION - MCP2515
// =============================================
bool initCAN() {
#ifdef ESP32
    serialPrintflnAlways("[CAN] Initializing SPI...");
    
    // Initialize SPI with MCP2515 pins
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, MCP_CS_PIN);
    delay(100);
    
    serialPrintflnAlways("[CAN] Initializing MCP2515 at %d kbps...", CAN_BAUDRATE / 1000);
    
    // Initialize MCP2515
    byte ret = CAN0.begin(MCP_ANY, CAN_BAUDRATE, MCP_OSC_FREQ);
    
    if (ret == CAN_OK) {
        serialPrintflnAlways("[CAN] MCP2515 initialized successfully");
        
        // Set to normal mode (not listen-only)
        CAN0.setMode(MCP_NORMAL);
        
        serialPrintflnAlways("[CAN] Mode set to NORMAL");
        
        // Initialize atomic variables
        realtimeVoltage.store(0.0f);
        realtimeCurrent.store(0.0f);
        realtimeUpdateTime.store(0);
        canMessageCount.store(0);
        canMessagesPerSecond.store(0);
        
        // Initialize charger variables
        chargerConnected.store(false);
        oriChargerDetected.store(false);
        lastChargerMsgTime.store(0);
        lastOriChargerMsgTime.store(0);
        isChargingMode.store(false);
        
        // Initialize system health
        lastSuccessfulLoop.store(millis());
        systemErrorCount.store(0);
        
        // Initialize local stats
        lastStatsTime = millis();
        localMsgCount = 0;
        
        return true;
    } else {
        serialPrintflnAlways("[CAN] ERROR: MCP2515 init failed, code: 0x%02X", ret);
        return false;
    }
#else
    return false;
#endif
}

// =============================================
// CHARGING MODE HELPER FUNCTIONS
// =============================================
#ifdef ESP32
bool isChargingModeActive() {
    return isChargingMode.load(std::memory_order_acquire);
}

bool isOriChargerActive() {
    return oriChargerDetected.load(std::memory_order_acquire);
}

uint32_t getChargerMessageAge() {
    if (!oriChargerDetected.load(std::memory_order_acquire)) {
        return 0xFFFFFFFF;
    }
    return millis() - lastOriChargerMsgTime.load(std::memory_order_acquire);
}

void setChargingMode(bool enable) {
    isChargingMode.store(enable, std::memory_order_release);
}

void updateSystemHealth() {
    lastSuccessfulLoop.store(millis(), std::memory_order_release);
}

uint32_t getSystemErrorCount() {
    return systemErrorCount.load(std::memory_order_acquire);
}

void resetSystemErrorCount() {
    systemErrorCount.store(0, std::memory_order_release);
}

bool isChargingPageEnabled() {
    return CHARGING_PAGE_ENABLED;
}

// =============================================
// SPAM MESSAGE DETECTION
// =============================================
bool isSpamChargerMessage(uint32_t id) {
    if (id == ORI_CHARGER_SPAM_ID) return true;
    if (id == CHARGER_DATA_ID_1 || id == CHARGER_DATA_ID_2) return true;
    if (id == BMS_CHARGING_FLAG) return true;
    return false;
}

// =============================================
// GET MODE STRING FROM BYTE
// =============================================
static const char* getModeStringFromByte(uint8_t modeByte) {
    if (modeByte == MODE_BYTE_PARK) return "PARK";
    else if (modeByte == MODE_BYTE_DRIVE) return "DRIVE";
    else if (modeByte == MODE_BYTE_SPORT) return "SPORT";
    else if (modeByte == MODE_BYTE_REVERSE) return "REVERSE";
    else if (modeByte == MODE_BYTE_CHARGING_1 || 
             modeByte == MODE_BYTE_CHARGING_2 || 
             modeByte == MODE_BYTE_CHARGING_3 || 
             modeByte == MODE_BYTE_CHARGING_4) return "CHARGING";
    else if (modeByte == MODE_BYTE_STANDBY_1 || 
             modeByte == MODE_BYTE_STANDBY_2 || 
             modeByte == MODE_BYTE_STANDBY_3) return "STAND";
    else if (modeByte == MODE_BYTE_CUTOFF_DRIVE || 
             modeByte == MODE_BYTE_CUTOFF_SPORT) return "BRAKE";
    else return "UNKNOWN";
}

// =============================================
// REAL-TIME CAN PARSING - MCP2515 VERSION
// =============================================
void parseCANMessage(unsigned long id, unsigned char len, unsigned char *rxBuf) {
    unsigned long receivedTime = millis();
    
    // Update system health
    lastSuccessfulLoop.store(receivedTime, std::memory_order_release);
    
    // Count total messages
    canMessageCount.fetch_add(1, std::memory_order_relaxed);
    localMsgCount++;
    
    // ========== CHARGER DETECTION ==========
    if (id == ORI_CHARGER_SPAM_ID || id == CHARGER_DATA_ID_1 || id == CHARGER_DATA_ID_2 || id == BMS_CHARGING_FLAG) {
        if (id == ORI_CHARGER_SPAM_ID) {
            oriChargerDetected.store(true, std::memory_order_release);
            lastOriChargerMsgTime.store(receivedTime, std::memory_order_release);
            oriChargerMessageCount.fetch_add(1, std::memory_order_relaxed);
            
            if (CHARGING_PAGE_ENABLED && !isChargingMode.load(std::memory_order_acquire)) {
                isChargingMode.store(true, std::memory_order_release);
            }
        } else {
            chargerConnected.store(true, std::memory_order_release);
            lastChargerMsgTime.store(receivedTime, std::memory_order_release);
            chargerMessageCount.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Update vehicle charger data
        vehicle.chargerConnected = chargerConnected.load();
        vehicle.oriChargerDetected = oriChargerDetected.load();
        vehicle.lastChargerMessage = receivedTime;
        
        // Jangan return untuk message yang juga mengandung data
        if (id != ID_VOLTAGE_CURRENT && id != ID_CTRL_MOTOR && id != ID_BATT_5S) {
            return;
        }
    }
    
    // ========== CHARGER DATA (0x1810D0F3 or 0x1811D0F3) ==========
    if ((id == 0x1810D0F3UL || id == 0x1811D0F3UL) && len >= 5) {
        uint16_t vRaw = (uint16_t)((rxBuf[0] << 8) | rxBuf[1]);
        vehicle.chargerVoltage = vRaw * 0.1f;
        
        uint16_t iRaw = (uint16_t)((rxBuf[2] << 8) | rxBuf[3]);
        vehicle.chargerCurrent = iRaw * 0.1f;
        
        vehicle.chargerStatus = rxBuf[4];
        vehicle.chargerConnected = true;
        vehicle.lastChargerMessage = receivedTime;
        return;
    }
    
    // ========== CONTROLLER BASIC (0x0A010810) ==========
    if(id == ID_CTRL_MOTOR && len >= 8) {
        uint8_t m = rxBuf[1];
        
        // Mode
        vehicle.lastModeByte = m;
        
        // RPM dan Speed
        vehicle.rpm = rxBuf[2] | (rxBuf[3] << 8);
        vehicle.speed = (int)(vehicle.rpm * 0.1033f); // Approx conversion
        
        // Temperatures
        vehicle.tempCtrl = rxBuf[4];
        vehicle.tempMotor = rxBuf[5];
        vehicle.lastMessageTime = receivedTime;
        return;
    }
    
    // ========== BMS TEMPERATURES (0x0E6C0D09) ==========
    if (id == ID_BATT_5S && len >= 5) {
        int sum = 0;
        for (int i = 0; i < 5; i++) {
            vehicle.cellTemps[i] = rxBuf[i];  // direct °C
            sum += (int)vehicle.cellTemps[i];
        }
        vehicle.tempBatt = sum / 5;
        vehicle.lastMessageTime = receivedTime;
        return;
    }
    
    // ========== VOLTAGE & CURRENT (0x0A6D0D09) ==========
    if(id == ID_VOLTAGE_CURRENT && len >= 8) {
        // Voltage
        uint16_t vRaw = (uint16_t)((rxBuf[0] << 8) | rxBuf[1]);
        vehicle.rawVoltageHex = vRaw;
        float voltage = vRaw * 0.1f;
        
        // Current (signed)
        uint16_t iRawU = (uint16_t)((rxBuf[2] << 8) | rxBuf[3]);
        vehicle.rawCurrentHex = iRawU;
        int16_t iRawS = (int16_t)iRawU;
        float current = iRawS * 0.1f;
        
        // Deadzone
        if(fabs(current) < CURRENT_DISPLAY_DEADZONE) {
            current = 0.0f;
        }
        
        // Capacity
        uint16_t remainCap = (uint16_t)((rxBuf[4] << 8) | rxBuf[5]);
        vehicle.remainingCapacity = remainCap * 0.1f;
        
        uint16_t fullCap = (uint16_t)((rxBuf[6] << 8) | rxBuf[7]);
        vehicle.fullCapacity = fullCap * 0.1f;
        
        // Atomic updates
        realtimeVoltage.store(voltage, std::memory_order_release);
        realtimeCurrent.store(current, std::memory_order_release);
        realtimeUpdateTime.store(receivedTime, std::memory_order_release);
        
        // Update vehicle data
        vehicle.batteryVoltage = voltage;
        vehicle.batteryCurrent = current;
        vehicle.chargingCurrent = (current > 1.0f);
        vehicle.lastMessageTime = receivedTime;
        
        return;
    }
    
    // ========== BATTERY HEALTH & SOC (0x0A6E0D09) ==========
    if(id == 0x0A6E0D09UL && len >= 6) {
        uint16_t socVal = (uint16_t)((rxBuf[0] << 8) | rxBuf[1]);
        vehicle.rawSOCHex = socVal;
        
        // Gunakan lookup table untuk SOC yang akurat
        vehicle.batterySOC = (int)getSoCFromLookup(socVal);
        if(vehicle.batterySOC > 100) vehicle.batterySOC = 100;
        if(vehicle.batterySOC < 0) vehicle.batterySOC = 0;
        
        uint16_t sohVal = (uint16_t)((rxBuf[2] << 8) | rxBuf[3]);
        vehicle.batterySOH = (int)(sohVal * 0.1f);
        if(vehicle.batterySOH > 100) vehicle.batterySOH = 100;
        
        vehicle.batteryCycleCount = (uint16_t)((rxBuf[4] << 8) | rxBuf[5]);
        vehicle.lastMessageTime = receivedTime;
        return;
    }
    
    // ========== CELL VOLTAGE STATS (0x0A6F0D09) ==========
    if(id == 0x0A6F0D09UL && len >= 8) {
        vehicle.cellHighestVolt = (uint16_t)((rxBuf[0] << 8) | rxBuf[1]);
        vehicle.cellHighestNum = rxBuf[2];
        vehicle.cellLowestVolt = (uint16_t)((rxBuf[3] << 8) | rxBuf[4]);
        vehicle.cellLowestNum = rxBuf[5];
        vehicle.cellAvgVolt = (uint16_t)((rxBuf[6] << 8) | rxBuf[7]);
        vehicle.cellDelta = vehicle.cellHighestVolt - vehicle.cellLowestVolt;
        vehicle.lastMessageTime = receivedTime;
        return;
    }
    
    // ========== TEMPERATURE STATS (0x0A700D09) ==========
    if(id == 0x0A700D09UL && len >= 6) {
        vehicle.tempMax = rxBuf[0];
        vehicle.tempMaxCell = rxBuf[1];
        vehicle.tempMin = rxBuf[4];
        vehicle.tempMinCell = rxBuf[5];
        vehicle.lastMessageTime = receivedTime;
        return;
    }
    
    // ========== BALANCE STATUS (0x0A730D09) ==========
    if(id == 0x0A730D09UL && len >= 6) {
        vehicle.balanceMode = rxBuf[0];
        vehicle.balanceStatus = rxBuf[1];
        vehicle.balanceBits[0] = rxBuf[2];
        vehicle.balanceBits[1] = rxBuf[3];
        vehicle.balanceBits[2] = rxBuf[4];
        vehicle.balanceBits[3] = rxBuf[5];
        
        char buff[20];
        snprintf(buff, sizeof(buff), "%02X %02X %02X %02X %02X %02X",
                 rxBuf[0], rxBuf[1], rxBuf[2], 
                 rxBuf[3], rxBuf[4], rxBuf[5]);
        vehicle.rawBalanceHex = String(buff);
        vehicle.lastMessageTime = receivedTime;
        return;
    }
    
    // ========== BMS CHARGING FLAG (0x0AB40D09) ==========
    if(id == BMS_CHARGING_FLAG && len >= 1) {
        bool isCharging = (rxBuf[0] == 0x01);
        if (isCharging && !isChargingMode.load(std::memory_order_acquire)) {
            isChargingMode.store(true, std::memory_order_release);
        } else if (!isCharging && isChargingMode.load(std::memory_order_acquire)) {
            // Don't immediately clear - wait for timeout
        }
        return;
    }
    
    // ========== CELL VOLTAGES BLOCKS (0x0E64-0x0E69) ==========
    if ((id & 0xFFF0FFFF) == 0x0E600D09) {
        int baseIndex = -1;
        switch (id) {
            case 0x0E640D09UL: baseIndex = 0;  break;
            case 0x0E650D09UL: baseIndex = 4;  break;
            case 0x0E660D09UL: baseIndex = 8;  break;
            case 0x0E670D09UL: baseIndex = 12; break;
            case 0x0E680D09UL: baseIndex = 16; break;
            case 0x0E690D09UL: baseIndex = 20; break;
            default: break;
        }
        if (baseIndex >= 0) {
            for (int i = 0; i < 4 && (baseIndex + i) < MAX_CELLS; i++) {
                int off = i * 2;
                if (off + 1 < len) {
                    vehicle.cellVoltages[baseIndex + i] = 
                        (uint16_t)((rxBuf[off] << 8) | rxBuf[off + 1]);
                }
            }
            
            // Update statistics setiap kali dapat data cell baru
            updateCellStatistics();
        }
        vehicle.lastMessageTime = receivedTime;
        return;
    }
}

// =============================================
// CAN TASK - MCP2515 VERSION (NON-BLOCKING)
// =============================================
void canTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    serialPrintflnAlways("[CAN] Task started on Core %d", xPortGetCoreID());
    
    while(true) {
        uint32_t taskDelay = CAN_UPDATE_RATE_DRIVING_MS;
        
        // Check for available CAN messages (non-blocking)
        unsigned char len = 0;
        unsigned char rxBuf[8];
        long unsigned int rxId;
        
        // Read all pending messages
        int processed = 0;
        while (CAN0.checkReceive() == CAN_MSGAVAIL && processed < 20) {
            if (CAN0.readMsgBuf(&rxId, &len, rxBuf) == CAN_OK) {
                rxId = rxId & 0x1FFFFFFF;  // Strip extended flag if needed
                parseCANMessage(rxId, len, rxBuf);
                processed++;
            } else {
                // Read error - break to avoid infinite loop
                break;
            }
            
            // Yield occasionally to prevent watchdog
            if (processed % 10 == 0) {
                taskYIELD();
            }
        }
        
        // Update CAN statistics every second
        uint32_t currentTime = millis();
        if(currentTime - lastStatsTime >= 1000) {
            canMessagesPerSecond.store(localMsgCount, std::memory_order_release);
            localMsgCount = 0;
            lastStatsTime = currentTime;
        }
        
        // Check charger timeout
        if (oriChargerDetected.load(std::memory_order_acquire)) {
            if (currentTime - lastOriChargerMsgTime.load(std::memory_order_acquire) > CHARGER_TIMEOUT_MS) {
                oriChargerDetected.store(false, std::memory_order_release);
                
                if (isChargingMode.load(std::memory_order_acquire)) {
                    isChargingMode.store(false, std::memory_order_release);
                }
            }
        }
        
        if (chargerConnected.load(std::memory_order_acquire)) {
            if (currentTime - lastChargerMsgTime.load(std::memory_order_acquire) > CHARGER_TIMEOUT_MS) {
                chargerConnected.store(false, std::memory_order_release);
            }
        }
        
        // Use adaptive delay
        if (isChargingMode.load(std::memory_order_acquire)) {
            taskDelay = CAN_UPDATE_RATE_CHARGING_MS;
        }
        
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(taskDelay));
    }
}
#endif

// =============================================
// REAL-TIME DATA GETTERS (ATOMIC)
// =============================================
float getRealtimeVoltage() {
#ifdef ESP32
    return realtimeVoltage.load(std::memory_order_acquire);
#else
    return vehicle.batteryVoltage;
#endif
}

float getRealtimeCurrent() {
#ifdef ESP32
    return realtimeCurrent.load(std::memory_order_acquire);
#else
    return vehicle.batteryCurrent;
#endif
}

unsigned long getRealtimeUpdateTime() {
#ifdef ESP32
    return realtimeUpdateTime.load(std::memory_order_acquire);
#else
    return vehicle.lastMessageTime;
#endif
}

bool isDataFresh() {
#ifdef ESP32
    unsigned long lastUpdate = realtimeUpdateTime.load(std::memory_order_acquire);
    uint32_t timeout = DATA_FRESH_TIMEOUT_NORMAL_MS;
    if (isChargingModeActive()) {
        timeout = DATA_FRESH_TIMEOUT_CHARGING_MS;
    }
    return (millis() - lastUpdate < timeout);
#else
    return (millis() - vehicle.lastMessageTime < 2000);
#endif
}

float getBatteryVoltage() {
    return getRealtimeVoltage();
}

float getBatteryCurrent() {
    return getRealtimeCurrent();
}

bool isChargerConnected() {
#ifdef ESP32
    return chargerConnected.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool isOriChargerDetected() {
#ifdef ESP32
    return oriChargerDetected.load(std::memory_order_acquire);
#else
    return false;
#endif
}

int getTempCtrl() {
    return vehicle.tempCtrl;
}

int getTempMotor() {
    return vehicle.tempMotor;
}

int getTempBatt() {
    return vehicle.tempBatt;
}

uint8_t getCurrentModeByte() {
    return vehicle.lastModeByte;
}

bool isSportMode() {
    return false;
}

bool isCruiseMode() {
    return false;
}

bool isCutoffMode() {
    return false;
}

uint8_t getBatterySOC() {
    return vehicle.batterySOC;
}

bool isChargingCurrent() {
    return vehicle.chargingCurrent;
}

void getBMSDataForDisplay(float &voltage, float &current, uint8_t &soc, bool &isCharging) {
    voltage = getRealtimeVoltage();
    current = getRealtimeCurrent();
    soc = vehicle.batterySOC;
    isCharging = isChargingCurrent();
}

uint32_t getCANMessageCount() {
#ifdef ESP32
    return canMessageCount.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

uint32_t getCANMessagesPerSecond() {
#ifdef ESP32
    return canMessagesPerSecond.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

void resetCANStatistics() {
#ifdef ESP32
    canMessageCount.store(0);
    canMessagesPerSecond.store(0);
#endif
}

void resetCANData() {
#ifdef ESP32
    realtimeVoltage.store(0.0f);
    realtimeCurrent.store(0.0f);
    realtimeUpdateTime.store(0);
    
    chargerConnected.store(false);
    oriChargerDetected.store(false);
    lastChargerMsgTime.store(0);
    lastOriChargerMsgTime.store(0);
    isChargingMode.store(false);
    
    resetCANStatistics();
#endif
    
    vehicle.batteryVoltage = 0.0f;
    vehicle.batteryCurrent = 0.0f;
    vehicle.tempCtrl = DEFAULT_TEMP;
    vehicle.tempMotor = DEFAULT_TEMP;
    vehicle.tempBatt = DEFAULT_TEMP;
    vehicle.lastModeByte = 0;
    vehicle.chargingCurrent = false;
    vehicle.lastMessageTime = 0;
}
