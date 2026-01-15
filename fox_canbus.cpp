#include "fox_canbus.h"
#include "fox_config.h"
#include "fox_vehicle.h"
#include <Arduino.h>

#ifdef ESP32
#include <driver/twai.h>
#endif

bool canInitialized = false;

bool foxCANInit() {
#ifdef ESP32
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CAN_TX_PIN,
        (gpio_num_t)CAN_RX_PIN,
        (twai_mode_t)CAN_MODE
    );
    
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        Serial.println("Gagal install CAN driver");
        return false;
    }
    
    if (twai_start() != ESP_OK) {
        Serial.println("Gagal start CAN bus");
        return false;
    }

    Serial.println("CAN siap");
    canInitialized = true;
    return true;
#else
    Serial.println("CAN hanya untuk ESP32");
    return false;
#endif
}

bool foxCANIsInitialized() {
    return canInitialized;
}

void foxCANUpdate() {
#ifdef ESP32
    twai_message_t message;
    
    if(twai_receive(&message, pdMS_TO_TICKS(50)) == ESP_OK) {
        // Kirim data ke vehicle module untuk diproses
        foxVehicleUpdateFromCAN(message.identifier, message.data, message.data_length_code);
    }
#endif
}
