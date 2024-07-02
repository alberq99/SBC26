#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "driver/i2c.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "config.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "sys/param.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_https_server.h"
#include "esp_tls.h"
#include "cJSON.h"
#include <stdint.h>
#include <stddef.h>
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"

//#define WIFI_SSID "Airbox-ACAF"
//#define WIFI_PASS "nC9z5q7YHZex"

#define WIFI_SSID "MiFibra-97A6"
#define WIFI_PASS "h7DsHGX4"

#define AP_SSID "SBC26"
#define AP_PASS "PASS12345"

#define relayPin 27 // Pin del relé para controlar la electroválvula
#define SENSOR_GPIO ADC_CHANNEL_6

#define ADC1_CHANNEL (ADC1_CHANNEL_7)  // Use ADC1 channel 0 (GPIO36)
#define ADC_WIDTH (ADC_WIDTH_BIT_12)   // ADC width 12 bits
#define ADC_ATTEN (ADC_ATTEN_DB_11)

#define DEFAULT_VREF    1100

#define MAX_RETRY  5

#define MQTT_URL "mqtt://demo.thingsboard.io"
#define TOKEN "VY0ZsH7AcYXAMq6nkJJl"  // Access token from ThingsBoard

int8_t water_state = 1;  // Nuevo estado para el control del agua
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_atten_t atten = ADC_ATTEN_DB_11;
static const adc_unit_t unit = ADC_UNIT_1;
static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int MQTT_CONNECTED_BIT = BIT1;
static int retry_num = 0;
bool valve_open = false;

uint32_t read_humidity_sensor();
esp_err_t init_water_control(void);
void toggle_water(void);
void toggle_no_water(void);
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data);


static const char *TAG = "main";
char estado_actual[20] = "Desconocido";

void initialize_led_pins() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL<<BLINK_GPIO_1),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&io_conf);
}

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
    ESP_LOGI(TAG, "Starting Web Server...");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret)
    {
        ESP_LOGI(TAG, "Error starting Web Server!");
        return NULL;
    }

    // Configurar los manejadores de URI
    ESP_LOGI(TAG, "Registering Uri Handlers");
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

void start_softap_server() {            //soft ap
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);
    esp_wifi_start();

    httpd_handle_t server = NULL;
    server = start_webserver();
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

    toggle_no_water();  //initially water off

    ESP_LOGI(TAG, "Inicialización del control de agua completada");
    return ESP_OK;
}

void toggle_water(void)
{
    gpio_set_level(relayPin, 0);
}

void toggle_no_water(void)
{
    gpio_set_level(relayPin, 1);
    valve_open = false;
}

void wifi_init_sta(void) {              // wifi mode
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    esp_wifi_connect();
}


// Handler for WiFi events
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_num < MAX_RETRY) {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG, "retrying to connect to the network, attempt %d", retry_num);
        } else {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGI(TAG, "failed to connect to the network");
            start_softap_server();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        httpd_handle_t server = NULL;
        server = start_webserver();
    }
}

static void check_efuse(void)
{
    // Check if Two Point or Vref are burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        ESP_LOGI(TAG, "eFuse Two Point: Supported");
    } else {
        ESP_LOGI(TAG, "eFuse Two Point: NOT supported");
    }

    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        ESP_LOGI(TAG, "eFuse Vref: Supported");
    } else {
        ESP_LOGI(TAG, "eFuse Vref: NOT supported");
    }
}

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        ESP_LOGI(TAG, "Characterized using Two Point Value");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        ESP_LOGI(TAG, "Characterized using eFuse Vref");
    } else {
        ESP_LOGI(TAG, "Characterized using Default Vref");
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        client = event->client;
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        while(1){
        uint32_t humidity = read_humidity_sensor();

        float moisture_percent = convert_to_percentage(humidity);

        printf("Humidity level: %.2f\n", moisture_percent);
    
        char payload[100];
        snprintf(payload, sizeof(payload), "{\"humidity\": %.2f}", moisture_percent);
        esp_mqtt_client_publish(client, "v1/devices/me/telemetry", payload, 0, 1, 0);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        client = NULL;
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URL,
        .credentials = {
            .username = "pasa55h3jvjkk13gjo78", // Your ThingsBoard device access token
            .client_id = "13ncszb521mqm6mog3b6",
            .authentication.password = "q4uwwki61kggybtlw6w3"
        },
        .session = {
            .keepalive = 120, // Keepalive interval in seconds
            .disable_clean_session = false // Set to true if you want to maintain session state across reconnects
        },
        .network = {
            .reconnect_timeout_ms = 10000, // Reconnect interval in milliseconds
        },
        .task = {
            .priority = 5, // Task priority (set according to your application's needs)
            .stack_size = 4096 // Task stack size in bytes
        },
        .buffer = {
            .size = 1024, // MQTT send/receive buffer size
            .out_size = 1024 // MQTT output buffer size
        }
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);

}

void adc_init(void)
{
    adc1_config_width(ADC_WIDTH);
    adc1_config_channel_atten(ADC1_CHANNEL, ADC_ATTEN);

    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH, DEFAULT_VREF, adc_chars);
}

uint32_t read_humidity_sensor()
{
    // Read ADC value
    uint32_t adc_reading = adc1_get_raw(ADC1_CHANNEL);

    if (adc_reading > 400 && adc_reading < 1400) {
        if (!valve_open) {
            printf("Alert! Dry Soil!\n");
            toggle_water();
            printf("Water ON!\n");
            vTaskDelay(20000 / portTICK_PERIOD_MS);         // 15seg apertura/cierre de la valvula + X segundo de irrigación (este caso 5seg)
            toggle_no_water();
        }
    }
    
    return adc_reading;
}

float convert_to_percentage(uint32_t adc_value) {
    const uint32_t dry_min = 400;
    const uint32_t dry_max = 1400;
    const uint32_t humid_min = 1400;
    const uint32_t humid_max = 2400;
    const uint32_t wet_min = 2400;

    if (adc_value < dry_min) {
        return 0.0; 
    } else if (adc_value <= dry_max) {
        return ((float)(adc_value - dry_min) / (dry_max - dry_min)) * 50.0;
    } else if (adc_value <= humid_max) {
        return 50.0 + ((float)(adc_value - humid_min) / (humid_max - humid_min)) * 50.0;
    } else {
        return 100.0;
    }
}

static void set_static_dns() {
    // Set DNS server 1
    ip_addr_t d;
    IP_ADDR4(&d, 8, 8, 8, 8); // Google's primary DNS server
    dns_setserver(0, &d);

    // Set DNS server 2
    IP_ADDR4(&d, 8, 8, 4, 4); // Google's secondary DNS server
    dns_setserver(1, &d);

    ESP_LOGI("DNS", "Static DNS Set to Google DNS");
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_water_control());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    set_static_dns();

    wifi_init_sta();

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI("main", "Connected to WiFi network");

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, NULL));

    // Initialize ADC characteristics
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, ADC_WIDTH_BIT_12, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
    check_efuse();
    adc_init();

    // MQTT Initialization
    mqtt_app_start();

    // Wait for MQTT connection before starting the sensor monitoring task
    xEventGroupWaitBits(s_wifi_event_group, MQTT_CONNECTED_BIT, false, true, portMAX_DELAY);
}
