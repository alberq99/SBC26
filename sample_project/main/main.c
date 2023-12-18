#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include "nrf24l01.h"
#include "rf24.h"

#define CE_PIN GPIO_NUM_2
#define CSN_PIN GPIO_NUM_15

void app_main() {
    nrf24_setup(CE_PIN, CSN_PIN);
    nrf24_set_rf(RF24_DATARATE_2MBPS, RF24_PA_HIGH);

    const uint64_t pipe = 0xF0F0F0F0E1LL;
    nrf24_open_reading_pipe(1, pipe);

    char text[32];

    while (1) {
        if (nrf24_available()) {
            nrf24_recv(text, sizeof(text));
            printf("Mensaje recibido: %s\n", text);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
