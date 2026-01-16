#ifndef FOX_DISPLAY_H
#define FOX_DISPLAY_H

#include <Arduino.h>
#include "fox_config.h" 
// Forward declaration saja
struct FoxVehicleData;

// Fungsi publik utama
void foxDisplayInit();
void foxDisplayUpdate(int page);
void foxDisplayShowSetupMode(bool blinkState);
void foxDisplayForceSportUpdate();
bool foxDisplayIsInitialized();

// Fungsi internal untuk tiap page
void displayPageClock();
void displayPageTemperature(const FoxVehicleData& vehicleData);
void displayPageElectrical(const FoxVehicleData& vehicleData);
void displayPageSport(const FoxVehicleData& vehicleData);

// Fungsi helper untuk sport page
void updateBlinkState();
void displayCruiseMode();
void displaySportModeLowSpeed();
void displaySportModeHighSpeed(uint8_t speedKmh);

#endif
