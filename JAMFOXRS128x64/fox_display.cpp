#include "fox_display.h"
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "fox_config.h"
#include "fox_rtc.h"
#include "fox_canbus.h"
#include "fox_serial.h"
#include "fox_page.h"
#include "fox_ble.h"
#include "fox_task.h"
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// =============================================
// GLOBAL VARIABLES
// =============================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool displayInitialized = false;
bool displayReady = false;

#ifdef ESP32
extern SemaphoreHandle_t i2cMutex;
QueueHandle_t displayQueue = NULL;
#endif

// =============================================
// I2C SAFETY VARIABLES
// =============================================
static volatile bool i2cOperationInProgress = false;
static unsigned long lastI2CFailure = 0;
static uint32_t i2cFailureCount = 0;

// =============================================
// ANIMATION VARIABLES
// =============================================
static float animatedVoltage = 0.0f;
static float animatedCurrent = 0.0f;
static float animatedPower = 0.0f;
static float targetVoltage = 0.0f;
static float targetCurrent = 0.0f;
static float targetPower = 0.0f;
static unsigned long lastAnimationUpdate = 0;
static bool animationInitialized = false;

static float currentVelocity = 0.0f;
static const float MAX_VELOCITY = 5.0f;
static const float ACCELERATION = 10.0f;
static const float DAMPING = 0.9f;

// =============================================
// DISPLAY STATE VARIABLES
// =============================================
bool appModeDisplayActive = false;
unsigned long lastDisplayUpdateTime = 0;

// =============================================
// EXTERNAL VARIABLES FROM BLE
// =============================================
extern volatile bool deviceConnected;

// =============================================
// MODE DISPLAY NAME - PAGE 1
// =============================================
String getModeDisplayName(uint8_t modeByte) {
    switch(modeByte) {
        case 0x00: return "PARK";
        case 0x70: return "DRIVE";
        case 0xB0: return "SPORT";
        case 0x40: return "READY";
        case 0x74: return "CRUISE";
        case 0xB4: return "CRUISE";
        case 0x72: return "BRAKE";
        case 0xB2: return "BRAKE";
        case 0x78:
        case 0x08:
        case 0xB8: return "STAND";
        case 0x50: return "REVERSE";
        case 0x61:
        case 0xA1:
        case 0xA9:
        case 0x69: return "CHARGING";
        default: return "STAND BY";
    }
}

// =============================================
// I2C RECOVERY FUNCTIONS
// =============================================
void recoverI2CBus() {
    serialPrintf("[I2C-RECOVERY] Starting recovery...\n");
    
    #ifdef ESP32
    if(i2cMutex != NULL) {
        xSemaphoreGive(i2cMutex);
    }
    #endif
    
    Wire.end();
    delay(100);
    
    #ifdef ESP32
    gpio_reset_pin((gpio_num_t)SDA_PIN);
    gpio_reset_pin((gpio_num_t)SCL_PIN);
    delay(10);
    #endif
    
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(I2C_CLOCK_SPEED);
    #ifdef ESP32
    Wire.setTimeOut(1000);
    #endif
    
    delay(100);
    
    for(int i = 0; i < 3; i++) {
        Wire.beginTransmission(OLED_ADDRESS);
        if(Wire.endTransmission() == 0) {
            serialPrintf("[I2C-RECOVERY] Success\n");
            
            if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, DISPLAY_BEGIN_RESET)) {
                displayInitialized = true;
                displayReady = true;
                display.clearDisplay();
                display.display();
                i2cFailureCount = 0;
            }
            break;
        }
        delay(50);
    }
    
    #ifdef ESP32
    if(i2cMutex != NULL) {
        xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100));
    }
    #endif
}

// =============================================
// HARD RESET I2C
// =============================================
void hardResetI2C() {
    serialPrintflnAlways("[I2C] HARD RESET - Pulling pins LOW");
    
    #ifdef ESP32
    pinMode(SDA_PIN, OUTPUT);
    pinMode(SCL_PIN, OUTPUT);
    digitalWrite(SDA_PIN, LOW);
    digitalWrite(SCL_PIN, LOW);
    delay(10);
    
    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, INPUT_PULLUP);
    delay(10);
    
    Wire.end();
    delay(100);
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(I2C_CLOCK_SPEED);
    
    if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, DISPLAY_BEGIN_RESET)) {
        displayInitialized = true;
        displayReady = true;
        serialPrintflnAlways("[I2C] HARD RESET successful");
    } else {
        serialPrintflnAlways("[I2C] HARD RESET failed - display not responding");
    }
    #endif
}

// =============================================
// SAFE I2C OPERATIONS
// =============================================
bool safeI2COperation(uint32_t timeoutMs) {
    if(!displayInitialized) return false;
    
    #ifdef ESP32
    if(i2cOperationInProgress) return false;
    i2cOperationInProgress = true;
    
    if(i2cMutex != NULL) {
        if(xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
            i2cOperationInProgress = false;
            return false;
        }
    }
    #endif
    
    bool success = false;
    uint8_t error = 5;
    
    for(int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(OLED_ADDRESS);
        error = Wire.endTransmission(true);
        if(error == 0) {
            success = true;
            break;
        }
        delay(5);
    }
    
    #ifdef ESP32
    if(i2cMutex != NULL) xSemaphoreGive(i2cMutex);
    i2cOperationInProgress = false;
    #endif
    
    if(!success) {
        lastI2CFailure = millis();
        i2cFailureCount++;
        
        if(i2cFailureCount >= 5) {
            recoverI2CBus();
            i2cFailureCount = 0;
        }
    }
    
    return success;
}

bool safeI2COperationWithBackoff(uint32_t timeoutMs) {
    if(!displayInitialized) return false;
    
    #ifdef ESP32
    if(i2cOperationInProgress) return false;
    i2cOperationInProgress = true;
    
    if(i2cMutex != NULL) {
        if(xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
            i2cOperationInProgress = false;
            return false;
        }
    }
    #endif
    
    bool success = false;
    int retryCount = 0;
    uint32_t delayMs = 10;
    
    while (retryCount < 3 && !success) {
        Wire.beginTransmission(OLED_ADDRESS);
        uint8_t error = Wire.endTransmission(true);
        
        if(error == 0) {
            success = true;
            break;
        }
        retryCount++;
        delay(delayMs);
        delayMs *= 2;
    }
    
    #ifdef ESP32
    if(i2cMutex != NULL) xSemaphoreGive(i2cMutex);
    i2cOperationInProgress = false;
    #endif
    
    if(!success) {
        lastI2CFailure = millis();
        i2cFailureCount++;
        
        if(i2cFailureCount >= 3) {
            hardResetI2C();
            i2cFailureCount = 0;
        }
    }
    
    return success;
}

void releaseI2C() {
    #ifdef ESP32
    if(i2cMutex != NULL) xSemaphoreGive(i2cMutex);
    #endif
    delayMicroseconds(50);
}

// =============================================
// HELPER FUNCTIONS
// =============================================
int calculateWidthFont2(const String& text) {
    int width = 0;
    for (unsigned int i = 0; i < text.length(); i++) {
        char c = text[i];
        if (c == '.' || c == ',') width += 6;
        else if (c == '+' || c == '-') width += 8;
        else width += 12;
    }
    return width;
}

String removeTrailingZero(float value, int decimalPlaces) {
    char buffer[20];
    dtostrf(value, 0, decimalPlaces, buffer);
    String result = String(buffer);
    if (result.indexOf('.') != -1) {
        while (result.endsWith("0")) result.remove(result.length() - 1);
        if (result.endsWith(".")) result.remove(result.length() - 1);
    }
    return result;
}

String formatVoltage(float voltage) {
    if (voltage < 0.05f && voltage > -0.05f) return "0";
    if (voltage < 100.0f) return removeTrailingZero(voltage, 1);
    else {
        char buffer[10];
        sprintf(buffer, "%.0f", voltage);
        return String(buffer);
    }
}

String formatCurrent(float current) {
    float absCurrent = fabs(current);
    if (absCurrent < 0.05f) return "0";
    if (absCurrent < 10.0f) return removeTrailingZero(absCurrent, 1);
    else if (absCurrent < 100.0f) return removeTrailingZero(absCurrent, 1);
    else {
        char buffer[10];
        sprintf(buffer, "%.0f", absCurrent);
        return String(buffer);
    }
}

String formatPower(float power) {
    float absPower = fabs(power);
    if (absPower < 0.05f) return "0";
    if (absPower < 10.0f) return removeTrailingZero(absPower, 1);
    else {
        char buffer[10];
        sprintf(buffer, "%.0f", absPower);
        return String(buffer);
    }
}

// =============================================
// ANIMATION FUNCTIONS
// =============================================
void resetAnimation() {
    animatedVoltage = 0.0f;
    animatedCurrent = 0.0f;
    animatedPower = 0.0f;
    targetVoltage = 0.0f;
    targetCurrent = 0.0f;
    targetPower = 0.0f;
    currentVelocity = 0.0f;
    lastAnimationUpdate = 0;
    animationInitialized = false;
}

void updateAnimationTargets() {
    float newVoltage = getRealtimeVoltage();
    float newCurrent = getRealtimeCurrent();
    
    float newPower = newVoltage * newCurrent;
    
    if(newPower > MAX_DISPLAY_POWER) newPower = MAX_DISPLAY_POWER;
    if(newPower < MIN_DISPLAY_POWER) newPower = MIN_DISPLAY_POWER;
    
    if(!animationInitialized) {
        targetVoltage = newVoltage;
        targetCurrent = newCurrent;
        targetPower = newPower;
        animatedVoltage = newVoltage;
        animatedCurrent = newCurrent;
        animatedPower = newPower;
        animationInitialized = true;
        return;
    }
    
    if(fabs(newVoltage - targetVoltage) > VOLTAGE_CHANGE_THRESHOLD) {
        targetVoltage = newVoltage;
    }
    
    if(fabs(newCurrent - targetCurrent) > CURRENT_CHANGE_THRESHOLD) {
        targetCurrent = newCurrent;
    }
    
    if(fabs(newPower - targetPower) > POWER_CHANGE_THRESHOLD) {
        targetPower = newPower;
    }
}

void updateAnimation() {
    unsigned long now = millis();
    if(now - lastAnimationUpdate < ANIMATION_INTERVAL_MS) return;
    
    float deltaTime = (now - lastAnimationUpdate) / 1000.0f;
    lastAnimationUpdate = now;
    
    if(fabs(targetVoltage - animatedVoltage) > 0.01f) {
        animatedVoltage = animatedVoltage + (targetVoltage - animatedVoltage) * ANIMATION_SMOOTHNESS;
        if(fabs(targetVoltage - animatedVoltage) < 0.05f) {
            animatedVoltage = targetVoltage;
        }
    }
    
    float currentDiff = targetCurrent - animatedCurrent;
    
    if(fabs(currentDiff) > CURRENT_CHANGE_THRESHOLD) {
        float acceleration = currentDiff * ACCELERATION;
        currentVelocity = currentVelocity * DAMPING + acceleration * deltaTime;
        
        if(currentVelocity > MAX_VELOCITY) currentVelocity = MAX_VELOCITY;
        if(currentVelocity < -MAX_VELOCITY) currentVelocity = -MAX_VELOCITY;
        
        animatedCurrent += currentVelocity * deltaTime;
        
        if(fabs(currentDiff) < 0.05f) {
            animatedCurrent = targetCurrent;
            currentVelocity = 0.0f;
        }
        
        if((targetCurrent > animatedCurrent && currentDiff < 0) || 
           (targetCurrent < animatedCurrent && currentDiff > 0)) {
            animatedCurrent = targetCurrent;
            currentVelocity = 0.0f;
        }
    } else {
        animatedCurrent = targetCurrent;
        currentVelocity = 0.0f;
    }
    
    if(fabs(targetPower - animatedPower) > 1.0f) {
        animatedPower = animatedPower + (targetPower - animatedPower) * ANIMATION_SMOOTHNESS;
        if(fabs(targetPower - animatedPower) < 5.0f) {
            animatedPower = targetPower;
        }
        if(animatedPower > MAX_DISPLAY_POWER) animatedPower = MAX_DISPLAY_POWER;
        if(animatedPower < MIN_DISPLAY_POWER) animatedPower = MIN_DISPLAY_POWER;
    }
}

bool isChargingPageDisplayed() {
    #ifdef ESP32
    return (isChargingModeActive() && CHARGING_PAGE_ENABLED);
    #else
    return false;
    #endif
}

// =============================================
// DISPLAY INITIALIZATION
// =============================================
void resetDisplayState() {
    if(!displayInitialized) return;
    display.setFont();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    display.setCursor(0, 0);
}

void showSplashScreen() {
    if(!displayInitialized) return;
    
    if(!safeI2COperation(I2C_MUTEX_TIMEOUT_MS)) {
        serialPrintf("[DISPLAY] Cannot show splash - I2C busy\n");
        return;
    }
    
    resetDisplayState();
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    const GFXfont* splashFont = NULL;
    
    #if !defined(DISABLE_LARGE_FONTS_INIT) || !DISABLE_LARGE_FONTS_INIT
    if(SPLASH_FONT_SIZE == 1) splashFont = &FreeSansBold9pt7b;
    else if(SPLASH_FONT_SIZE == 2) splashFont = &FreeSansBold12pt7b;
    #endif
    
    if(splashFont != NULL) {
        display.setFont(splashFont);
    }
    
    int textWidth = strlen(SPLASH_TEXT) * (splashFont == NULL ? 6 : 10);
    int xPos = (SCREEN_WIDTH - textWidth) / 2;
    int yPos = SPLASH_POS_Y;
    
    display.setCursor(xPos, yPos);
    display.print(SPLASH_TEXT);
    
    display.display();
    releaseI2C();
    
    delay(SPLASH_DURATION_MS);
    resetDisplayState();
}

void initDisplay() {
    serialPrintf("[DISPLAY] Starting initialization for 0.96 inch...\n");
    
    Wire.end();
    delay(50);
    
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(I2C_CLOCK_SPEED);
    #ifdef ESP32
    Wire.setTimeOut(1000);
    #endif
    delay(50);
    
    serialPrintf("[DISPLAY] Scanning I2C...\n");
    uint8_t error;
    int foundDevices = 0;
    for(uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        error = Wire.endTransmission();
        if(error == 0) {
            serialPrintf("[DISPLAY] I2C device found at 0x%02X\n", addr);
            foundDevices++;
            if(addr == OLED_ADDRESS) {
                serialPrintf("[DISPLAY] OLED found at 0x%02X\n", addr);
            }
        }
    }
    if(foundDevices == 0) {
        serialPrintf("[DISPLAY] No I2C devices found!\n");
        displayInitialized = false;
        displayReady = false;
        return;
    }
    
    bool initSuccess = false;
    
    serialPrintf("[DISPLAY] Attempt 1: Standard init\n");
    if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        initSuccess = true;
        serialPrintf("[DISPLAY] Standard init SUCCESS\n");
    }
    
    if(!initSuccess) {
        serialPrintf("[DISPLAY] Attempt 2: Init with reset=false\n");
        if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, false)) {
            initSuccess = true;
            serialPrintf("[DISPLAY] Init with reset=false SUCCESS\n");
        }
    }
    
    if(!initSuccess) {
        serialPrintf("[DISPLAY] Attempt 3: Init with reset=true\n");
        if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true)) {
            initSuccess = true;
            serialPrintf("[DISPLAY] Init with reset=true SUCCESS\n");
        }
    }
    
    if(!initSuccess) {
        serialPrintf("[DISPLAY] Attempt 4: Reset I2C and retry\n");
        Wire.end();
        delay(100);
        Wire.begin(SDA_PIN, SCL_PIN);
        Wire.setClock(I2C_CLOCK_SPEED);
        delay(50);
        
        if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, false)) {
            initSuccess = true;
            serialPrintf("[DISPLAY] Reset + init SUCCESS\n");
        }
    }
    
    if(initSuccess) {
        displayInitialized = true;
        displayReady = true;
        
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        
        showSplashScreen();
        
        display.clearDisplay();
        display.display();
        
        resetAnimation();
        serialPrintf("[DISPLAY] 0.96 inch initialized successfully\n");
        return;
    }
    
    displayInitialized = false;
    displayReady = false;
    serialPrintf("[DISPLAY] ERROR: Failed to initialize display\n");
}

void resetDisplayFont() {
    if (!displayInitialized) return;
    
    if (safeI2COperation(I2C_MUTEX_TIMEOUT_MS)) {
        display.setFont();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setTextWrap(false);
        display.setCursor(0, 0);
        releaseI2C();
    }
}

// =============================================
// APP MODE DISPLAY FUNCTIONS
// =============================================
void updateAppModeDisplay() {
    if (!displayInitialized || !displayReady) return;
    
    #ifdef ESP32
    if (isBLEConnected()) {
        return;
    }
    #endif
    
    if (safeI2COperationWithBackoff(I2C_MUTEX_TIMEOUT_MS)) {
        display.clearDisplay();
        
        // "APP MODE"
        display.setFont(APP_MODE_FONT);
        display.setTextSize(1);
        display.setCursor(APP_MODE_X, APP_MODE_Y);
        display.print("APP MODE");
        
        // "waiting..."
        display.setFont();
        display.setTextSize(1);
        int waitingWidth = strlen(APP_MODE_WAITING_TEXT) * 6;
        int waitingX = (SCREEN_WIDTH - waitingWidth) / 2;
        display.setCursor(waitingX, APP_MODE_STATUS_Y);
        display.print(APP_MODE_WAITING_TEXT);
        
        display.display();
        releaseI2C();
    }
}

void showAppModeDisplay() {
    updateAppModeDisplay();
}

void showBleOffDisplay() {
    if (!displayReady) return;
    
    if (safeI2COperation(I2C_MUTEX_TIMEOUT_MS)) {
        display.setFont();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setTextWrap(false);
        
        display.clearDisplay();
        
        // "BLE OFF"
        display.setFont(BLE_OFF_FONT);
        display.setTextSize(1);
        display.setCursor(BLE_OFF_X, BLE_OFF_Y);
        display.print(BLE_OFF_TEXT);
        
        // "Disconnected"
        display.setFont();
        display.setTextSize(1);
        int statusWidth = strlen(BLE_OFF_STATUS_TEXT) * 6;
        int statusX = (SCREEN_WIDTH - statusWidth) / 2;
        display.setCursor(statusX, BLE_OFF_STATUS_Y);
        display.print(BLE_OFF_STATUS_TEXT);
        
        display.display();
        releaseI2C();
        
        serialPrintfln("[DISPLAY] BLE OFF shown");
    }
}

// =============================================
// TRANSITION TO CLOCK (PAGE 1)
// =============================================
void transitionFromAppModeToClock() {
    if (!displayInitialized || !displayReady) return;
    
    serialPrintfln("[DISPLAY] Starting transition to Clock Page");
    
    if (safeI2COperation(I2C_MUTEX_TIMEOUT_MS)) {
        display.setFont();
        display.setTextSize(1);
        
        display.clearDisplay();
        display.display();
        delay(20);
        
        resetDisplayState();
        delay(10);
        
        RTCDateTime dt = getRTC();
        
        // Jam
        display.setFont(PAGE1_TIME_FONT);
        display.setCursor(PAGE1_TIME_X, PAGE1_TIME_Y);
        display.printf("%02d:%02d", dt.hour, dt.minute);
        
        // Hari, Tanggal, Tahun
        display.setFont();
        display.setTextSize(1);
        
        int hariIndex = dt.dayOfWeek - 1;
        if(hariIndex < 0) hariIndex = 0;
        if(hariIndex > 6) hariIndex = 6;
        
        int bulanIndex = dt.month - 1;
        if(bulanIndex < 0) bulanIndex = 0;
        if(bulanIndex > 11) bulanIndex = 11;
        
        display.setCursor(PAGE1_DAY_X, PAGE1_DAY_Y);
        display.print(DAY_NAMES[hariIndex]);
        
        display.setCursor(PAGE1_DATE_X, PAGE1_DATE_Y);
        display.printf("%d %s", dt.day, MONTH_NAMES[bulanIndex]);
        
        display.setCursor(PAGE1_YEAR_X, PAGE1_YEAR_Y);
        display.printf("%04d", dt.year);
        
        display.display();
        delay(10);
        display.display();
        
        releaseI2C();
        
        serialPrintfln("[DISPLAY] Transition complete");
    } else {
        serialPrintfln("[DISPLAY] ERROR: Cannot get I2C for transition");
    }
}

// =============================================
// UPDATE DISPLAY FUNCTIONS
// =============================================
bool safeDisplayUpdate(int page) {
    if(!displayInitialized || !displayReady) return false;
    updateDisplay(page);
    return true;
}

void resetDisplay() {
    if(!displayInitialized) return;
    if(safeI2COperation(I2C_MUTEX_TIMEOUT_MS)) {
        resetDisplayState();
        display.clearDisplay();
        display.display();
        releaseI2C();
    }
}

void updateDisplay(int page) {
    if(!displayInitialized) return;
    
    if (!safeI2COperation(10)) {
        serialPrintfln("[DISPLAY] I2C busy, skipping update");
        return;
    }
    
    resetDisplayState();
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    #ifdef ESP32
    if(isChargingModeActive() && CHARGING_PAGE_ENABLED) {
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.print("LOCKED");
        display.display();
        releaseI2C();
        return;
    }
    #endif
    
    if(page == 1) {
        RTCDateTime dt = getRTC();
        
        #ifdef ESP32
        bool bleActive = isInAppMode();
        #else
        bool bleActive = false;
        #endif
        
        String modeName;
        
        if (bleActive) {
            modeName = PAGE1_BLE_ON_TEXT;
        } else {
            uint8_t modeByte = getCurrentModeByte();
            modeName = getModeDisplayName(modeByte);
        }
        
        // Mode / BLE ON
        display.setFont(PAGE1_MODE_FONT);
        display.setTextSize(1);
        display.setCursor(PAGE1_MODE_X, PAGE1_MODE_Y);
        display.print(modeName);
        
        // Jam
        display.setFont(PAGE1_TIME_FONT);
        display.setTextSize(1);
        
        String timeStr = String(dt.hour) + ":" + String(dt.minute);
        if(dt.minute < 10) timeStr = String(dt.hour) + ":0" + String(dt.minute);
        if(dt.hour < 10) timeStr = "0" + timeStr;
        
        display.setCursor(PAGE1_TIME_X, PAGE1_TIME_Y);
        display.print(timeStr);
        
        // Hari, Tanggal, Tahun
        display.setFont();
        display.setTextSize(1);
        
        int hariIndex = dt.dayOfWeek - 1;
        if(hariIndex < 0) hariIndex = 0;
        if(hariIndex > 6) hariIndex = 6;
        
        int bulanIndex = dt.month - 1;
        if(bulanIndex < 0) bulanIndex = 0;
        if(bulanIndex > 11) bulanIndex = 11;
        
        display.setCursor(PAGE1_DAY_X, PAGE1_DAY_Y);
        display.print(DAY_NAMES[hariIndex]);
        
        display.setCursor(PAGE1_DATE_X, PAGE1_DATE_Y);
        display.printf("%d %s", dt.day, MONTH_NAMES[bulanIndex]);
        
        display.setCursor(PAGE1_YEAR_X, PAGE1_YEAR_Y);
        display.printf("%04d", dt.year);
        
    } else if(page == 2) {
        float tempEcu = getTempCtrl();
        float tempMotor = getTempMotor();
        float tempBatt = getTempBatt();
        
        // ---- BARIS 1: ECU ----
        display.setFont(PAGE2_LABEL_FONT);
        display.setTextSize(1);
        display.setCursor(PAGE2_LABEL_X, PAGE2_ROW1_Y);
        display.print(PAGE2_LABEL1);
        
        String val1 = String((int)tempEcu);
        int val1Width = val1.length() * 10;
        int val1X = SCREEN_WIDTH - val1Width - PAGE2_VALUE_PADDING;
        display.setCursor(val1X, PAGE2_ROW1_Y);
        display.print(val1);
        
        display.setFont(PAGE2_DEGREE_FONT);
        display.setTextSize(1);
        display.setCursor(val1X + val1Width + PAGE2_DEGREE_OFFSET_X, PAGE2_ROW1_Y + PAGE2_DEGREE_OFFSET_Y);
        display.print(PAGE2_DEGREE_SYMBOL);
        
        // ---- BARIS 2: BLDC ----
        display.setFont(PAGE2_LABEL_FONT);
        display.setTextSize(1);
        display.setCursor(PAGE2_LABEL_X, PAGE2_ROW2_Y);
        display.print(PAGE2_LABEL2);
        
        String val2 = String((int)tempMotor);
        int val2Width = val2.length() * 10;
        int val2X = SCREEN_WIDTH - val2Width - PAGE2_VALUE_PADDING;
        display.setCursor(val2X, PAGE2_ROW2_Y);
        display.print(val2);
        
        display.setFont(PAGE2_DEGREE_FONT);
        display.setTextSize(1);
        display.setCursor(val2X + val2Width + PAGE2_DEGREE_OFFSET_X, PAGE2_ROW2_Y + PAGE2_DEGREE_OFFSET_Y);
        display.print(PAGE2_DEGREE_SYMBOL);
        
        // ---- BARIS 3: BATT ----
        display.setFont(PAGE2_LABEL_FONT);
        display.setTextSize(1);
        display.setCursor(PAGE2_LABEL_X, PAGE2_ROW3_Y);
        display.print(PAGE2_LABEL3);
        
        String val3 = String((int)tempBatt);
        int val3Width = val3.length() * 10;
        int val3X = SCREEN_WIDTH - val3Width - PAGE2_VALUE_PADDING;
        display.setCursor(val3X, PAGE2_ROW3_Y);
        display.print(val3);
        
        display.setFont(PAGE2_DEGREE_FONT);
        display.setTextSize(1);
        display.setCursor(val3X + val3Width + PAGE2_DEGREE_OFFSET_X, PAGE2_ROW3_Y + PAGE2_DEGREE_OFFSET_Y);
        display.print(PAGE2_DEGREE_SYMBOL);
        
    } else if(page == 3) {
        #ifdef ESP32
        if(!isChargingModeActive()) {
            updateAnimationTargets();
            updateAnimation();
        }
        #endif
        
        float displayVoltage = animatedVoltage;
        float displayCurrent = animatedCurrent;
        
        // ---- VOLTASE ----
        String voltageStr = formatVoltage(displayVoltage);
        
        display.setFont(PAGE3_VOLTAGE_FONT);
        display.setTextSize(1);
        
        int voltWidth = voltageStr.length() * 18;
        int voltX = (SCREEN_WIDTH - voltWidth) / 2;
        display.setCursor(voltX, PAGE3_VOLTAGE_Y);
        display.print(voltageStr);
        
        display.setFont(PAGE3_UNIT_FONT);
        display.setCursor(voltX + voltWidth + PAGE3_UNIT_OFFSET_X, PAGE3_VOLTAGE_Y);
        display.print(PAGE3_VOLTAGE_UNIT);
        
        // ---- ARUS ----
        String currentStr = formatCurrent(displayCurrent);
        float deadzone = CURRENT_DISPLAY_DEADZONE;
        #ifdef ESP32
        if(isChargingModeActive()) deadzone = CHARGING_CURRENT_DEADZONE;
        #endif
        
        bool isNegative = (displayCurrent < -deadzone);
        bool isPositive = (displayCurrent > deadzone);
        
        String displayCurrentStr = "";
        if (isPositive) {
            displayCurrentStr = "+" + currentStr;
        } else if (isNegative) {
            displayCurrentStr = "-" + currentStr;
        } else {
            displayCurrentStr = currentStr;
        }
        
        display.setFont(PAGE3_CURRENT_FONT);
        display.setTextSize(1);
        
        int currentWidth = displayCurrentStr.length() * 18;
        int currentX = (SCREEN_WIDTH - currentWidth) / 2;
        display.setCursor(currentX, PAGE3_CURRENT_Y);
        display.print(displayCurrentStr);
        
        display.setFont(PAGE3_UNIT_FONT);
        display.setCursor(currentX + currentWidth + PAGE3_UNIT_OFFSET_X, PAGE3_CURRENT_Y);
        display.print(PAGE3_CURRENT_UNIT);
        
        // ---- DATA FRESH INDICATOR ----
        bool dataFresh = isDataFresh();
        if(!dataFresh) {
            display.setFont();
            display.setTextSize(1);
            display.setCursor(PAGE3_FRESH_X, PAGE3_FRESH_Y);
            display.print(PAGE3_FRESH_SYMBOL);
        }
        
    } else if(page == 4) {
        #ifdef ESP32
        if(!isChargingModeActive()) {
            updateAnimationTargets();
            updateAnimation();
        }
        #endif
        
        float displayVoltage = animatedVoltage;
        float displayCurrent = animatedCurrent;
        float displayPower = animatedPower;
        
        if(displayPower > MAX_DISPLAY_POWER) displayPower = MAX_DISPLAY_POWER;
        if(displayPower < MIN_DISPLAY_POWER) displayPower = MIN_DISPLAY_POWER;
        
        // ---- BARIS 1: VOLTASE & ARUS ----
        display.setFont(PAGE4_TOP_FONT);
        display.setTextSize(1);
        
        String voltStr = formatVoltage(displayVoltage);
        String currentStr = formatCurrent(displayCurrent);
        float deadzone = CURRENT_DISPLAY_DEADZONE;
        #ifdef ESP32
        if(isChargingModeActive()) deadzone = CHARGING_CURRENT_DEADZONE;
        #endif
        
        bool isNegative = (displayCurrent < -deadzone);
        bool isPositive = (displayCurrent > deadzone);
        
        String displayCurrentStr = "";
        if (isPositive) {
            displayCurrentStr = "+" + currentStr;
        } else if (isNegative) {
            displayCurrentStr = "-" + currentStr;
        } else {
            displayCurrentStr = currentStr;
        }
        
        String line1 = voltStr + "V  " + displayCurrentStr + "A";
        
        int line1Width = line1.length() * 10;
        int line1X = (SCREEN_WIDTH - line1Width) / 2;
        display.setCursor(line1X, PAGE4_TOP_Y);
        display.print(line1);
        
        // ---- BARIS 2: DAYA ----
        String powerStr = formatPower(displayPower);
        
        String displayPowerStr = "";
        if (displayPower > 0.1f) {
            displayPowerStr = "+" + powerStr;
        } else if (displayPower < -0.1f) {
            displayPowerStr = "-" + powerStr;
        } else {
            displayPowerStr = powerStr;
        }
        
        display.setFont(PAGE4_POWER_FONT);
        display.setTextSize(1);
        
        int powerWidth = displayPowerStr.length() * 18;
        int powerX = (SCREEN_WIDTH - powerWidth) / 2;
        display.setCursor(powerX, PAGE4_POWER_Y);
        display.print(displayPowerStr);
        
        // ---- BARIS 3: "watt" ----
        display.setFont(PAGE4_WATT_FONT);
        display.setTextSize(1);
        
        int wattWidth = strlen(PAGE4_WATT_TEXT) * 10;
        int wattX = (SCREEN_WIDTH - wattWidth) / 2;
        display.setCursor(wattX, PAGE4_WATT_Y);
        display.print(PAGE4_WATT_TEXT);
        
        // ---- DATA FRESH INDICATOR ----
        bool dataFresh = isDataFresh();
        if(!dataFresh) {
            display.setFont();
            display.setTextSize(1);
            display.setCursor(PAGE3_FRESH_X, PAGE3_FRESH_Y);
            display.print(PAGE3_FRESH_SYMBOL);
        }
        
    } else {
        updateDisplay(1);
        releaseI2C();
        return;
    }
    
    display.display();
    releaseI2C();
}

void showSetupMode(bool blinkState) {
    if(!displayInitialized) return;
    resetDisplayState();
    display.clearDisplay();
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(SSD1306_WHITE);
    
    int xPos = SETUP_MODE_POS_X;
    int yPos = SETUP_MODE_POS_Y;
    
    if(SETUP_MODE_POS_X == 0) {
        int textWidth = strlen(SETUP_TEXT) * 10;
        xPos = (SCREEN_WIDTH - textWidth) / 2;
        yPos = 22;
    }
    
    if(blinkState) {
        display.setCursor(xPos, yPos);
        display.print(SETUP_TEXT);
    }
    
    display.display();
}

bool isDisplayInitialized() {
    return displayInitialized;
}

// =============================================
// DISPLAY TASK FUNCTIONS
// =============================================
void initDisplayTask() {
    #ifdef ESP32
    if (!DISPLAY_TASK_ENABLED) return;
    
    displayQueue = xQueueCreate(DISPLAY_QUEUE_SIZE, sizeof(DisplayCommand));
    
    if (displayQueue == NULL) {
        serialPrintflnAlways("[DISPLAY] ERROR: Failed to create display queue");
        return;
    }
    
    serialPrintflnAlways("[DISPLAY] Display queue created");
    #endif
}

void sendDisplayCommand(DisplayCommandType type, int page, bool blinkState) {
    #ifdef ESP32
    if (displayQueue == NULL) return;
    
    DisplayCommand cmd;
    cmd.type = type;
    cmd.page = page;
    cmd.blinkState = blinkState;
    cmd.timestamp = millis();
    
    if (xQueueSend(displayQueue, &cmd, 0) != pdTRUE) {
        serialPrintfln("[DISPLAY] Queue full, dropping command %d", type);
    }
    #endif
}

bool isDisplayTaskBusy() {
    #ifdef ESP32
    if (displayQueue == NULL) return false;
    return (uxQueueMessagesWaiting(displayQueue) > 0);
    #else
    return false;
    #endif
}

void displayTask(void *pvParameters) {
    #ifdef ESP32
    TickType_t xLastWakeTime = xTaskGetTickCount();
    DisplayCommand cmd;
    unsigned long lastUpdateTime = 0;
    bool inAppMode = false;
    bool showingBleOff = false;
    unsigned long bleOffStartTime = 0;
    
    serialPrintflnAlways("[DISPLAY] Task started on Core %d", xPortGetCoreID());
    
    int waitCount = 0;
    while (!displayReady && waitCount < 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waitCount++;
    }
    
    if (!displayReady) {
        serialPrintflnAlways("[DISPLAY] ERROR: Display not ready after waiting");
    }
    
    if (displayReady) {
        if (safeI2COperationWithBackoff(I2C_MUTEX_TIMEOUT_MS)) {
            display.clearDisplay();
            display.display();
            serialPrintflnAlways("[DISPLAY] Initial clear done");
        }
    }
    
    while (true) {
        if (xQueueReceive(displayQueue, &cmd, 0) == pdTRUE) {
            if (!displayReady) continue;
            
            switch (cmd.type) {
                case DISPLAY_CMD_UPDATE_PAGE:
                    if (!showingBleOff) {
                        safeI2COperationWithBackoff(I2C_MUTEX_TIMEOUT_MS);
                        updateDisplay(cmd.page);
                        lastUpdateTime = millis();
                    }
                    break;
                    
                case DISPLAY_CMD_UPDATE_CLOCK:
                    if (inAppMode && !showingBleOff && !isBLEConnected()) {
                        updateAppModeDisplay();
                        lastUpdateTime = millis();
                    }
                    break;
                    
                case DISPLAY_CMD_TRANSITION_TO_CLOCK:
                    if (!showingBleOff) {
                        inAppMode = false;
                        transitionFromAppModeToClock();
                        lastUpdateTime = millis();
                    }
                    break;
                    
                case DISPLAY_CMD_CLEAR:
                    if (!showingBleOff) {
                        if (safeI2COperationWithBackoff(I2C_MUTEX_TIMEOUT_MS)) {
                            display.clearDisplay();
                            display.display();
                        }
                    }
                    break;
                    
                case DISPLAY_CMD_SHOW_BLE_OFF:
                    showBleOffDisplay();
                    showingBleOff = true;
                    bleOffStartTime = millis();
                    break;
                    
                case DISPLAY_CMD_RESET:
                    hardResetI2C();
                    break;
                    
                default:
                    break;
            }
        }
        
        if (displayReady) {
            unsigned long now = millis();
            
            if (showingBleOff) {
                if (now - bleOffStartTime >= 3000) {
                    showingBleOff = false;
                    inAppMode = isInAppMode();
                    if (!inAppMode) {
                        transitionFromAppModeToClock();
                    }
                }
            } else {
                uint32_t updateInterval = inAppMode ? DISPLAY_APP_MODE_UPDATE_MS : DISPLAY_UPDATE_INTERVAL_MS;
                
                if (now - lastUpdateTime >= updateInterval) {
                    if (inAppMode && !isBLEConnected()) {
                        updateAppModeDisplay();
                    } else if (!inAppMode) {
                        safeI2COperationWithBackoff(I2C_MUTEX_TIMEOUT_MS);
                        updateDisplay(currentPage);
                    }
                    lastUpdateTime = now;
                }
            }
        }
        
        if (!showingBleOff) {
            inAppMode = isInAppMode();
        }
        
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
    #endif
}