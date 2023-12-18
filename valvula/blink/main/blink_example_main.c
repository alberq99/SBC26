#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>

#define RELAY_GPIO_PIN GPIO_NUM_27

void initialize_relay() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
}

void turn_relay_on() {
    gpio_set_level(RELAY_GPIO_PIN, 1);
}

void turn_relay_off() {
    gpio_set_level(RELAY_GPIO_PIN, 0);
}

void relay_control_task(void *pvParameter) {
    while (1) {
        turn_relay_on();
        vTaskDelay(5000 / portTICK_RATE_MS);

        turn_relay_off();
        vTaskDelay(5000 / portTICK_RATE_MS);
    }
}

void app_main() {
    initialize_relay();

    xTaskCreate(&relay_control_task, "relay_control_task", 2048, NULL, 5, NULL);
}
