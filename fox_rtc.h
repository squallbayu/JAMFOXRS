#ifndef FOX_RTC_H
#define FOX_RTC_H

#include <Arduino.h>

struct RTCDateTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t dayOfWeek;
};

bool foxRTCInit();
RTCDateTime foxRTCGetDateTime();
String foxRTCGetTimeString(bool includeSeconds = true);
String foxRTCGetDateString();
void foxRTCSetTime(uint16_t year, uint8_t month, uint8_t day, 
                   uint8_t hour, uint8_t minute, uint8_t second,
                   uint8_t dayOfWeek = 0);
void foxRTCSetFromCompileTime();
bool foxRTCSetTimeFromString(String timeStr);
bool foxRTCSetDateFromString(String dateStr);
bool foxRTCSetDayOfWeek(uint8_t dayOfWeek);
float foxRTCGetTemperature();
bool foxRTCIsRunning();
void foxRTCDebugPrint();

#endif
