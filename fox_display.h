#ifndef FOX_DISPLAY_H
#define FOX_DISPLAY_H

#include <Arduino.h>

// Forward declaration saja
struct FoxVehicleData;

void foxDisplayInit();
void foxDisplayUpdate(int page);
void foxDisplayShowSetupMode(bool blinkState);
void foxDisplayForceSportUpdate();
bool foxDisplayIsInitialized();

// Fungsi internal - gunakan forward declaration
void displayPageClock();
void displayPageTemperature(const FoxVehicleData& vehicleData);
void displayPageSport(const FoxVehicleData& vehicleData);
void updateBlinkState();
void displayCruiseMode();
void displaySportModeLowSpeed();
void displaySportModeHighSpeed(uint8_t speedKmh);

#endif
