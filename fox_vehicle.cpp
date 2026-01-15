#include "fox_vehicle.h"
#include "fox_config.h"
#include <Arduino.h>

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
    .voltageValid = false
};

bool captureUnknownCAN = false;

void foxVehicleInit() {
    Serial.println("Vehicle module initialized");
    vehicleData.lastUpdate = millis();
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
    else {
        captureUnknownCANData(canId, data, len);
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
    vehicleData.speedKmh = data[3];
    vehicleData.speedValid = true;
    
    static uint8_t lastSpeed = 0;
    if(vehicleData.speedKmh != lastSpeed) {
        Serial.print("Speed: ");
        Serial.print(vehicleData.speedKmh);
        Serial.println(" km/h");
        lastSpeed = vehicleData.speedKmh;
    }
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
