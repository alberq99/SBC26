#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include <esp_https_server.h>
#include "esp_tls.h"
#include <string.h>
#include "driver/gpio.h"
#include <stdio.h>

#define relayPin 27 // Pin del relé para controlar la electroválvula

int8_t water_state = 0;  // Nuevo estado para el control del agua

static const char *TAG = "main";

esp_err_t init_water_control(void);
void toggle_water(void);
void toggle_no_water(void);
void control_water_valve(int state);

/* Un manejador HTTP GET */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    extern unsigned char view_start[] asm("_binary_view_html_start");
    extern unsigned char view_end[] asm("_binary_view_html_end");
    size_t view_len = view_end - view_start;
    char viewHtml[view_len];
    memcpy(viewHtml, view_start, view_len);
    ESP_LOGI(TAG, "URI: %s", req->uri);

    if (strcmp(req->uri, "/?water") == 0)
    {
        toggle_water();
    }
    if (strcmp(req->uri, "/?no-water") == 0)
    {
        toggle_no_water();
    }

    char *viewHtmlUpdated;
    int formattedStrResult = asprintf(&viewHtmlUpdated, viewHtml, water_state ? "ON" : "OFF", water_state ? "No Water" : "Water");

    httpd_resp_set_type(req, "text/html");

    if (formattedStrResult > 0)
    {
        httpd_resp_send(req, viewHtmlUpdated, view_len);
        free(viewHtmlUpdated);
    }
    else
    {
        ESP_LOGE(TAG, "Error actualizando las variables");
        httpd_resp_send(req, viewHtml, view_len);
    }

    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;

    // Iniciar el servidor httpd
    ESP_LOGI(TAG, "Iniciando servidor");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret)
    {
        ESP_LOGI(TAG, "Error al iniciar el servidor!");
        return NULL;
    }

    // Configurar los manejadores de URI
    ESP_LOGI(TAG, "Registrando manejadores de URI");
    httpd_register_uri_handler(server, &root);
    return server;
}

static void stop_webserver(httpd_handle_t server)
{
    // Detener el servidor httpd
    httpd_ssl_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server)
    {
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL)
    {
        *server = start_webserver();
    }
}

esp_err_t init_water_control(void)
{
    gpio_config_t pGPIOConfig;
    pGPIOConfig.pin_bit_mask = (1ULL << relayPin);
    pGPIOConfig.mode = GPIO_MODE_DEF_OUTPUT;
    pGPIOConfig.pull_up_en = GPIO_PULLUP_DISABLE;
    pGPIOConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pGPIOConfig.intr_type = GPIO_INTR_DISABLE;

    gpio_config(&pGPIOConfig);

    ESP_LOGI(TAG, "Inicialización del control de agua completada");
    return ESP_OK;
}

void toggle_water(void)
{
    // Implementar la lógica para manejar el botón "Water" aquí
    // Puedes cambiar el estado del agua o realizar acciones relacionadas con el riego
    water_state = !water_state;
    control_water_valve(water_state);
}

void toggle_no_water(void)
{
    // Implementar la lógica para manejar el botón "No Water" aquí
    // Puedes cambiar el estado sin agua o realizar acciones relacionadas con el riego
    water_state = 0;  // Por simplicidad, asumir que "No Water" siempre apaga el agua
    control_water_valve(water_state);
}

void control_water_valve(int state)
{
    // Implementar el control de la electroválvula aquí
    gpio_set_level(relayPin, water_state);
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_water_control());
    static httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    ESP_ERROR_CHECK(example_connect());
}
