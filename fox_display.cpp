#include "fox_display.h"
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "fox_config.h"
#include "fox_rtc.h"
#include "fox_vehicle.h"
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

// Deklarasi global
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool displayInitialized = false;

// Variabel untuk tracking perubahan
uint8_t lastDisplayedSpeed = 0;
uint16_t lastDisplayedRPM = 0;
bool sportNeedsUpdate = false;
unsigned long lastBlinkTime = 0;
bool blinkState = true;

void foxDisplayInit() {
    Serial.println("Initializing OLED...");
    
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(100);
    
    if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println("OLED FAILED!");
        displayInitialized = false;
        return;
    }
    
    Serial.println("OLED OK!");
    displayInitialized = true;
    
    // Splash screen menggunakan konfigurasi
    display.clearDisplay();
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 18);
    display.print(SPLASH_TEXT);
    display.display();
    
    delay(SPLASH_DURATION_MS);
    display.clearDisplay();
    display.display();
    
    // Reset ke font default
    display.setFont();
    display.setTextSize(FONT_SIZE_SMALL);
}

bool foxDisplayIsInitialized() {
    return displayInitialized;
}

void foxDisplayUpdate(int page) {
    if(!displayInitialized) return;
    
    FoxVehicleData vehicleData = foxVehicleGetData();
    
    // Untuk SPORT page: cek apakah data berubah
    if(page == PAGE_SPORT) {
        bool speedChanged = (vehicleData.speedKmh != lastDisplayedSpeed);
        bool rpmChanged = (vehicleData.rpm != lastDisplayedRPM);
        
        // Skip update jika tidak ada perubahan
        if(!speedChanged && !rpmChanged && !sportNeedsUpdate) {
            return;
        }
        
        // Update nilai terakhir
        lastDisplayedSpeed = vehicleData.speedKmh;
        lastDisplayedRPM = vehicleData.rpm;
        sportNeedsUpdate = false;
    }
    
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setFont();
    display.setTextSize(FONT_SIZE_SMALL);
    
    if(page == PAGE_CLOCK) {
        displayPageClock();
    }
    else if(page == PAGE_TEMP) {
        displayPageTemperature(vehicleData);
    }
    else if(page == PAGE_SPORT) {
        displayPageSport(vehicleData);
    }
    
    display.display();
}

void displayPageClock() {
    RTCDateTime dt = foxRTCGetDateTime();
    
    // Jam besar menggunakan konfigurasi format
    display.setFont(&FreeSansBold18pt7b);
    char timeStr[6];
    snprintf(timeStr, sizeof(timeStr), CLOCK_TIME_FORMAT, dt.hour, dt.minute);
    display.setCursor(0, 28);
    display.print(timeStr);
    
    // Info tanggal menggunakan konfigurasi
    display.setFont();
    display.setTextSize(FONT_SIZE_SMALL);
    
    const char* hari[] = {"MINGGU", "SENIN", "SELASA", "RABU", "KAMIS", "JUMAT", "SABTU"};
    const char* bulan[] = {"JAN", "FEB", "MAR", "APR", "MEI", "JUN", 
                          "JUL", "AGU", "SEP", "OKT", "NOV", "DES"};
    
    int hariIndex = dt.dayOfWeek - 1;
    if(hariIndex < 0) hariIndex = 0;
    if(hariIndex > 6) hariIndex = 6;
    
    int bulanIndex = dt.month - 1;
    if(bulanIndex < 0) bulanIndex = 0;
    if(bulanIndex > 11) bulanIndex = 11;
    
    int rightCol = 88;
    
    display.setCursor(rightCol, 5);
    display.print(hari[hariIndex]);
    
    char tanggalBulan[10];
    snprintf(tanggalBulan, sizeof(tanggalBulan), CLOCK_DATE_FORMAT, dt.day, bulan[bulanIndex]);
    display.setCursor(rightCol, 15);
    display.print(tanggalBulan);
    
    char yearStr[5];
    snprintf(yearStr, sizeof(yearStr), CLOCK_YEAR_FORMAT, dt.year);
    display.setCursor(rightCol, 25);
    display.print(yearStr);
}

void displayPageTemperature(const FoxVehicleData& vehicleData) {
    // Label menggunakan konfigurasi
    display.setTextSize(FONT_SIZE_SMALL);
    display.setCursor(0, 4);
    display.print(TEMP_LABEL_ECU);
    display.setCursor(43, 4);
    display.print(TEMP_LABEL_MOTOR);
    display.setCursor(86, 4);
    display.print(TEMP_LABEL_BATT);
    
    // Angka suhu
    display.setTextSize(FONT_SIZE_MEDIUM);
    display.setCursor(0, 16);
    display.print(vehicleData.tempController);
    display.setCursor(43, 16);
    display.print(vehicleData.tempMotor);
    display.setCursor(86, 16);
    display.print(vehicleData.tempBattery);
}

void displayPageSport(const FoxVehicleData& vehicleData) {
    // Deteksi mode
    bool isCruiseMode = (vehicleData.mode == MODE_CRUISE || 
                        vehicleData.mode == MODE_SPORT_CRUISE);
    bool isSportMode = (vehicleData.mode == MODE_SPORT || 
                       vehicleData.mode == MODE_SPORT_CRUISE);
    
    // PRIORITAS 1: CRUISE MODE
    if(isCruiseMode) {
        updateBlinkState();
        displayCruiseMode();
    }
    // PRIORITAS 2: SPORT MODE
    else if(isSportMode) {
        if(vehicleData.speedKmh < SPEED_TRIGGER_SPORT_PAGE) {
            displaySportModeLowSpeed();
        } else {
            displaySportModeHighSpeed(vehicleData.speedKmh);
        }
    }
}

void updateBlinkState() {
    if(millis() - lastBlinkTime > BLINK_INTERVAL_MS) {
        blinkState = !blinkState;
        lastBlinkTime = millis();
    }
}

void displayCruiseMode() {
    // "CRUISE" besar di tengah (blinking) menggunakan konfigurasi
    display.setTextSize(FONT_SIZE_LARGE);
    int textWidth = strlen(CRUISE_TEXT) * 18;
    int xPos = (SCREEN_WIDTH - textWidth) / 2;
    int yPos = 4;
    
    display.setCursor(xPos, yPos);
    if(blinkState) {
        display.print(CRUISE_TEXT);
    }
}

void displaySportModeLowSpeed() {
    // "SPORT" besar di tengah menggunakan konfigurasi
    display.setTextSize(FONT_SIZE_LARGE);
    int textWidth = strlen(SPORT_TEXT) * 18;
    int xPos = (SCREEN_WIDTH - textWidth) / 2;
    int yPos = 4;
    
    display.setCursor(xPos, yPos);
    display.print(SPORT_TEXT);
}

void displaySportModeHighSpeed(uint8_t speedKmh) {
    // "SPORT MODE" kecil di tengah atas menggunakan konfigurasi
    display.setTextSize(FONT_SIZE_SMALL);
    int sportWidth = strlen(SPORT_MODE_LABEL) * 6;
    int sportX = (SCREEN_WIDTH - sportWidth) / 2;
    display.setCursor(sportX, POS_TOP);
    display.print(SPORT_MODE_LABEL);
    
    // Angka speed besar
    display.setTextSize(FONT_SIZE_LARGE);
    char speedStr[4];
    snprintf(speedStr, sizeof(speedStr), "%3d", speedKmh);
    display.setCursor(0, 10);
    display.print(speedStr);
    
    // "km/h" di kanan angka menggunakan konfigurasi
    display.setTextSize(FONT_SIZE_SMALL);
    int speedWidth = 54;
    int kmhX = speedWidth + 4;
    int kmhY = 18;
    display.setCursor(kmhX, kmhY);
    display.print(KMH_TEXT);
}

void foxDisplayForceSportUpdate() {
    sportNeedsUpdate = true;
}

void foxDisplayShowSetupMode(bool blinkState) {
    if(!displayInitialized) return;
    
    display.clearDisplay();
    display.setTextSize(FONT_SIZE_MEDIUM);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 10);
    if (blinkState) {
        display.print(SETUP_TEXT);
    }
    display.display();
}
