#include "fox_vehicle.h"
#include "fox_config.h"
#include <Arduino.h>

// Lookup table SOC to BMS value (0-100%)
const uint16_t socToBms[101] = {
    0, 60,70,80,90,95,105,115,125,135,140,150,160,170,180,185,195,205,215,225,
    230,240,250,260,270,275,285,295,305,315,320,330,340,350,360,365,375,385,395,405,
    410,420,430,440,450,455,465,475,485,495,500,510,520,530,540,550,555,565,575,585,
    590,600,610,620,630,635,645,655,665,675,680,690,700,710,720,725,735,745,755,765,
    770,780,790,800,810,815,825,835,845,855,860,870,880,890,900,905,915,925,935,945,950
};

// Global variable
FoxVehicleData vehicleData = {
    .mode = MODE_UNKNOWN,
    .sportActive = false,
    .rpm = 0,
    .speedKmh = 0,
    .throttlePercent = 0,
    .tempController = DEFAULT_TEMP,
    .tempMotor = DEFAULT_TEMP,
    .tempBattery = DEFAULT_TEMP,
    .voltage = 0.0,
    .current = 0.0,
    .soc = 0,
    .lastUpdate = 0,
    .rpmValid = false,
    .speedValid = false,
    .tempValid = false,
    .voltageValid = false,
    .socValid = false
};

bool captureUnknownCAN = false;

// Function prototypes untuk internal functions
uint8_t bmsToSOC(uint16_t bmsValue);
void parseSOC(const uint8_t* data, uint8_t len);
void logUnknownMode(uint8_t modeByte);
void logModeChange(uint8_t modeByte);

void foxVehicleInit() {
    Serial.println("Vehicle module initialized");
    vehicleData.lastUpdate = millis();
}

// Fungsi: Convert BMS raw value to SOC percentage
uint8_t bmsToSOC(uint16_t bmsValue) {
    // Handle boundary cases
    if(bmsValue >= socToBms[100]) return 100;  // >=950 = 100%
    if(bmsValue <= socToBms[0]) return 0;      // <=0 = 0%
    
    // Linear search dalam lookup table
    for(int soc = 1; soc <= 100; soc++) {
        if(bmsValue == socToBms[soc]) {
            return soc;  // Exact match
        }
        else if(bmsValue < socToBms[soc]) {
            // Interpolasi antara soc-1 dan soc
            uint16_t prevValue = socToBms[soc-1];
            uint16_t currValue = socToBms[soc];
            
            // Linear interpolation
            float ratio = (float)(bmsValue - prevValue) / (currValue - prevValue);
            return soc - 1 + (uint8_t)(ratio + 0.5);  // Round to nearest integer
        }
    }
    
    return 0; // Fallback
}

// Fungsi parseSOC yang baru
void parseSOC(const uint8_t* data, uint8_t len) {
    if(len < 2) return;
    
    // Bytes 0-1 sebagai int16 (little-endian: byte0 LSB, byte1 MSB)
    uint16_t bmsValue = (data[1] << 8) | data[0];
    
    // Debug logging (hanya jika berubah)
    static uint16_t lastBmsValue = 0;
    static uint8_t lastSOC = 0;
    
    if(bmsValue != lastBmsValue) {
        uint8_t socPercent = bmsToSOC(bmsValue);
        
        if(socPercent != lastSOC) {
            Serial.print("SOC UPDATE: BMS=");
            Serial.print(bmsValue);
            Serial.print(" -> ");
            Serial.print(socPercent);
            Serial.println("%");
            
            // Update vehicle data
            vehicleData.soc = socPercent;
            vehicleData.socValid = true;
            lastSOC = socPercent;
        }
        
        lastBmsValue = bmsValue;
    }
}

// Fungsi utama
void foxVehicleUpdateFromCAN(uint32_t canId, const uint8_t* data, uint8_t len) {
    vehicleData.lastUpdate = millis();
    
    if(canId == FOX_CAN_MODE_STATUS && len >= 8) {
        parseModeStatus(data, len);
    }
    else if(canId == FOX_CAN_TEMP_CTRL_MOT && len >= 6) {
        parseSpeedAndTemp(data, len);
    }
    else if(canId == FOX_CAN_TEMP_BATT_5S && len >= 5) {
        parseBatteryTemp5S(data, len);
    }
    else if(canId == FOX_CAN_TEMP_BATT_SGL && len >= 6) {
        parseBatteryTempSingle(data, len);
    }
    else if(canId == FOX_CAN_VOLTAGE_CURRENT && len >= 4) {
        parseVoltageCurrent(data, len);
    }
    else if(canId == FOX_CAN_SOC && len >= 2) {
        parseSOC(data, len);
    }
    else if(canId == FOX_CAN_BMS_INFO) {
        // Ignore BMS info message (0x0A740D09)
        if(captureUnknownCAN) {
            Serial.print("BMS INFO IGNORED - ID: 0x");
            Serial.println(canId, HEX);
        }
    }
    else {
        captureUnknownCANData(canId, data, len);
    }
}

// PARSING VOLTAGE DAN CURRENT TANPA SERIAL LOGGING
void parseVoltageCurrent(const uint8_t* data, uint8_t len) {
    if(len >= 4) {
        // ========================================
        // VOLTAGE (Bytes 0-1)
        // ========================================
        uint16_t voltageRaw = (data[0] << 8) | data[1];
        float newVoltage = voltageRaw * 0.1f;
        
        // ========================================
        // CURRENT (Bytes 2-3)
        // ========================================
        uint16_t rawCurrent = (data[2] << 8) | data[3];
        bool isDischarge = (rawCurrent & 0x8000) != 0;
        float newCurrent;
        
        if (isDischarge) {
            uint16_t complement = (0x10000 - rawCurrent);
            newCurrent = -(complement * 0.1f);
        } else {
            newCurrent = rawCurrent * 0.1f;
        }
        
        // Deadzone
        if (fabs(newCurrent) < BMS_DEADZONE_CURRENT) {
            newCurrent = 0.0f;
        }
        
        // Update data tanpa logging serial
        bool voltageChanged = fabs(newVoltage - vehicleData.voltage) > BMS_UPDATE_THRESHOLD_VOLTAGE;
        bool currentChanged = fabs(newCurrent - vehicleData.current) > BMS_UPDATE_THRESHOLD_CURRENT;
        
        if (voltageChanged || currentChanged) {
            vehicleData.voltage = newVoltage;
            vehicleData.current = newCurrent;
            vehicleData.voltageValid = true;
        }
    }
}

// Fungsi helper - IMPLEMENTASI
void parseModeStatus(const uint8_t* data, uint8_t len) {
    uint8_t modeByte = data[1];
    
    switch(modeByte) {
        case MODE_BYTE_PARK: vehicleData.mode = MODE_PARK; break;
        case MODE_BYTE_DRIVE: vehicleData.mode = MODE_DRIVE; break;
        case MODE_BYTE_SPORT: vehicleData.mode = MODE_SPORT; break;
        case MODE_BYTE_CRUISE: vehicleData.mode = MODE_CRUISE; break;
        case MODE_BYTE_SPORT_CRUISE: vehicleData.mode = MODE_SPORT_CRUISE; break;
        case MODE_BYTE_CUTOFF_1: 
        case MODE_BYTE_CUTOFF_2: vehicleData.mode = MODE_CUTOFF; break;
        case MODE_BYTE_STANDBY_1: 
        case MODE_BYTE_STANDBY_2: vehicleData.mode = MODE_STANDBY; break;
        default: 
            vehicleData.mode = MODE_UNKNOWN;
            logUnknownMode(modeByte);
            break;
    }
    
    vehicleData.rpm = data[2] | (data[3] << 8);
    vehicleData.rpmValid = true;
    
    vehicleData.tempController = data[4];
    vehicleData.tempMotor = data[5];
    vehicleData.tempValid = true;
    
    vehicleData.sportActive = (vehicleData.mode == MODE_SPORT || 
                               vehicleData.mode == MODE_SPORT_CRUISE);
    
    logModeChange(modeByte);
}

void parseSpeedAndTemp(const uint8_t* data, uint8_t len) {
    // Hanya parsing, tanpa logging speed yang mengganggu
    vehicleData.speedKmh = data[3];
    vehicleData.speedValid = true;
}

void parseBatteryTemp5S(const uint8_t* data, uint8_t len) {
    uint8_t maxTemp = 0;
    for(int i = 0; i < 5; ++i) {
        if(data[i] > maxTemp) maxTemp = data[i];
    }
    vehicleData.tempBattery = maxTemp;
    vehicleData.tempValid = true;
}

void parseBatteryTempSingle(const uint8_t* data, uint8_t len) {
    uint8_t battTemp = data[5];
    if(battTemp > vehicleData.tempBattery) {
        vehicleData.tempBattery = battTemp;
        vehicleData.tempValid = true;
    }
}

void captureUnknownCANData(uint32_t canId, const uint8_t* data, uint8_t len) {
    if(canId != 0x00000000 && canId != 0xFFFFFFFF && captureUnknownCAN) {
        Serial.print("UNKNOWN CAN ID: 0x");
        Serial.print(canId, HEX);
        Serial.print(" Len:");
        Serial.print(len);
        Serial.print(" Data:");
        
        for(int i = 0; i < len && i < 8; i++) {
            Serial.print(" ");
            if(data[i] < 0x10) Serial.print("0");
            Serial.print(data[i], HEX);
        }
        Serial.println();
    }
}

void logUnknownMode(uint8_t modeByte) {
    if(modeByte != MODE_BYTE_CRUISE && modeByte != MODE_BYTE_SPORT_CRUISE) {
        Serial.print("UNKNOWN MODE Byte: 0x");
        if(modeByte < 0x10) Serial.print("0");
        Serial.print(modeByte, HEX);
        Serial.print(" (");
        Serial.print(modeByte);
        Serial.println(")");
    }
}

void logModeChange(uint8_t modeByte) {
    static FoxVehicleMode lastMode = MODE_UNKNOWN;
    if(vehicleData.mode != lastMode) {
        Serial.print("Mode: ");
        Serial.print(foxVehicleModeToString(vehicleData.mode));
        Serial.print(" (Byte: 0x");
        if(modeByte < 0x10) Serial.print("0");
        Serial.print(modeByte, HEX);
        Serial.println(")");
        lastMode = vehicleData.mode;
    }
}

// Fungsi publik lainnya
FoxVehicleData foxVehicleGetData() {
    return vehicleData;
}

bool foxVehicleIsSportMode() {
    return vehicleData.sportActive;
}

bool foxVehicleDataIsFresh(unsigned long timeoutMs) {
    return (millis() - vehicleData.lastUpdate) < timeoutMs;
}

String foxVehicleModeToString(FoxVehicleMode mode) {
    switch(mode) {
        case MODE_PARK: return "PARK";
        case MODE_DRIVE: return "DRIVE";
        case MODE_SPORT: return "SPORT";
        case MODE_CUTOFF: return "CUTOFF";
        case MODE_STANDBY: return "STAND";
        case MODE_REVERSE: return "REVERSE";
        case MODE_NEUTRAL: return "NEUTRAL";
        case MODE_CRUISE: return "DRIVE+CRUISE";
        case MODE_SPORT_CRUISE: return "SPORT+CRUISE";
        default: return "UNKNOWN";
    }
}

void foxVehicleEnableUnknownCapture(bool enable) {
    captureUnknownCAN = enable;
    if(enable) {
        Serial.println("=== UNKNOWN CAN CAPTURE ENABLED ===");
        Serial.println("Aktifkan cruise control dan lihat CAN ID");
        Serial.println("================================");
    } else {
        Serial.println("Unknown CAN capture disabled");
    }
}
