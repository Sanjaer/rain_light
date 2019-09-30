/* LwIP SNTP example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/apps/sntp.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "driver/gpio.h"
#include "sdkconfig.h"

/* The project uses simple WiFi and PIN configuration 
 * that can be set via 'make menuconfig'.           
 */
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD
#ifdef CONFIG_BLINK_GPIO
#define BLINK_GPIO CONFIG_BLINK_GPIO
#else
#define BLINK_GPIO GPIO_NUM_2
#endif
#ifdef CONFIG_BLUE_GPIO
#define BLUE_GPIO CONFIG_BLUE_GPIO
#else
#define BLUE_GPIO GPIO_NUM_25
#endif
#ifdef CONFIG_GREEN_GPIO
#define GREEN_GPIO CONFIG_GREEN_GPIO
#else
#define GREEN_GPIO GPIO_NUM_26
#endif
#ifdef CONFIG_RED_GPIO
#define RED_GPIO CONFIG_RED_GPIO
#else
#define RED_GPIO GPIO_NUM_27
#endif
#ifdef CONFIG_HOUR_WAKE_UP
#define HOUR_WAKE_UP CONFIG_HOUR_WAKE_UP
#else
#define HOUR_WAKE_UP 7
#endif
#ifdef CONFIG_MINUTE_WAKE_UP
#define MINUTE_WAKE_UP CONFIG_MINUTE_WAKE_UP
#else
#define MINUTE_WAKE_UP 0
#endif
#ifdef CONFIG_TIME_AWAKE
#define TIME_AWAKE CONFIG_TIME_AWAKE
#else
#define TIME_AWAKE 10   // In minutes
#endif

/* Server connection's data */
#define WEB_SERVER "api.openweathermap.org"
#define WEB_PORT 80
#define WEB_URL "/data/2.5/weather?id=3110044&appid=285b57947a5bba66f588665b0a84291e"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
 * but we only care about one event - are we connected
 * to the AP with an IP?                            
 */
const int CONNECTED_BIT = BIT0;

/* Tags for logs */
static const char *TAG_SNTP = "sntp";
static const char *TAG_CTRL = "ctrl";

/* HTTP Request for the weather */
static const char *REQUEST = "GET " WEB_URL " HTTP/1.0\r\n"
    "Host: "WEB_SERVER"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";


/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;
static char http_response[1024];


static void obtain_time(void);
static void initialize_sntp(void);
static void initialise_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);
static void sleep_seconds(const uint32_t deep_sleep_sec);
static void configure_GPIOS(void);
static void control_GPIOs(uint8_t red_state, uint8_t green_state, uint8_t blue_state);
static uint32_t get_time_to_sleep();


/* Tasks' declarations */
void sntp_task(void *pvParameter);
void control_task(void *pvParameter);
void led_indicator_task(void *pvParameter);


/* Functions to be used by tasks*/
static void obtain_time(void){

    ESP_ERROR_CHECK( nvs_flash_init() );
    initialise_wifi();
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    uint8_t retry = 0;
    const uint8_t retry_count = 10;
    while(timeinfo.tm_year < (2018- 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG_SNTP, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    ESP_ERROR_CHECK( esp_wifi_stop() );
}


static void initialize_sntp(void) {

    ESP_LOGI(TAG_SNTP, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

}

static void initialise_wifi(void) {

    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    
    ESP_LOGI(TAG_SNTP, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );

}

static esp_err_t event_handler(void *ctx, system_event_t *event){

    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}


static void sleep_seconds(const uint32_t deep_sleep_sec) {
    
    ESP_LOGI(TAG_CTRL, "Entering deep sleep for %d seconds", deep_sleep_sec);
    esp_deep_sleep(1000000LL * deep_sleep_sec);

}

static void configure_GPIOS(void){

    gpio_pad_select_gpio(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(BLUE_GPIO);
    gpio_set_direction(BLUE_GPIO, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(GREEN_GPIO);
    gpio_set_direction(GREEN_GPIO, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(RED_GPIO);
    gpio_set_direction(RED_GPIO, GPIO_MODE_OUTPUT);

}

static void control_GPIOs(uint8_t red_state, uint8_t green_state, uint8_t blue_state){

    const TickType_t xDelay = (TIME_AWAKE*60000) / portTICK_PERIOD_MS;

    gpio_set_level(RED_GPIO, red_state);
    gpio_set_level(GREEN_GPIO, green_state);
    gpio_set_level(BLUE_GPIO, blue_state);
    vTaskDelay(xDelay);
    gpio_set_level(BLUE_GPIO, 0);
    gpio_set_level(RED_GPIO, 0);
    gpio_set_level(GREEN_GPIO, 0);

}


static uint32_t get_time_to_sleep(void){

    time_t now;
    struct tm timeinfo;
    int32_t hours_left, minutes_left;

    time(&now);
    localtime_r(&now, &timeinfo);

    printf("(HOUR_WAKE_UP)%d - (timeinfo.tm_hour)%d\n", HOUR_WAKE_UP, timeinfo.tm_hour);

    if ((hours_left = HOUR_WAKE_UP - timeinfo.tm_hour) < 0){
        hours_left = 23 + hours_left;
        printf ("negative hours %d\n", hours_left);
    }

    printf ("MINUTE_WAKE_UP(%d) - timeinfo.tm_min(%d)\n", MINUTE_WAKE_UP, timeinfo.tm_min); 

    if ((minutes_left = MINUTE_WAKE_UP - timeinfo.tm_min) < 0){
        minutes_left = 60 + minutes_left;

        printf ("negative minutes %d\n", minutes_left);
    }

    return hours_left * 3600 + minutes_left * 60;

}

/* Tasks */
void led_indicator_task(void *pvParameter){

    for(;;){
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay((uint32_t) pvParameter / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay((uint32_t) pvParameter / portTICK_PERIOD_MS);
    }    

}

void control_task(void *pvParameter){

    const struct addrinfo hints = {
        .ai_family = AF_INET,           // IPv4
        .ai_socktype = SOCK_STREAM,     // TCP
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r, err;
    char recv_buf[64];

    TaskHandle_t xHandleLed = NULL;

    // Create task for LED indicator control
    xTaskCreate(&led_indicator_task, "led_indicator_task", configMINIMAL_STACK_SIZE, (void*)250, 5, &xHandleLed);

    while(1) {
       
        // Wait for the callback to set the CONNECTED_BIT in the event group.
        ESP_ERROR_CHECK( nvs_flash_init() );
        initialise_wifi();
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                            false, true, portMAX_DELAY);
        ESP_LOGI(TAG_CTRL, "Connected to AP");

        // DNS Lookup
        err = getaddrinfo(WEB_SERVER, "80", &hints, &res);

        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG_CTRL, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        // Print the resolved IP. inet_ntoa is non-reentrant, 
        // look at ipaddr_ntoa_r for "real" code
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG_CTRL, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        // Allocate socket
        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG_CTRL, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG_CTRL, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG_CTRL, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG_CTRL, "... connected");
        freeaddrinfo(res);

        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG_CTRL, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG_CTRL, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG_CTRL, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG_CTRL, "... set socket receiving timeout success");

        // Read HTTP response
        uint32_t c=0;
        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            // print response
            for(int i = 0; i < r; i++) {
                http_response[c++]=recv_buf[i];
                putchar(http_response[c-1]);
            }
        } while(r > 0);

        http_response[c] = '\0';

        ESP_LOGI(TAG_CTRL, "\nhttp_response=%s\r\n", http_response);

// {"coord":{"lon":-1.98,"lat":43.31},"weather":[{"id":500,"main":"Rain","description":"light rain","icon":"10n"}],"base":"stations","main":{"temp":278.845,"pressure":996.07,"humidity":92,"temp_min":278.845,"temp_max":278.845,"sea_level":1042.02,"grnd_level":996.07},"wind":{"speed":0.92,"deg":194},"rain":{"3h":0.267},"clouds":{"all":100},"dt":1549304289,"sys":{"message":0.0055,"country":"ES","sunrise":1549264793,"sunset":1549301064},"id":3110044,"name":"San Sebastian","cod":200}

        char *weather;
        weather = strstr(http_response, ",\"main\":\"");
        weather = strtok(weather, ",");

        ESP_LOGI(TAG_CTRL, "\nweather=%s\r\n", weather);

        ESP_LOGI(TAG_CTRL, "\n... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
        close(s);

        vTaskDelete(xHandleLed);
        gpio_set_level(BLINK_GPIO, 1);

        if (strcmp(weather, "\"main\":\"Rain\"") == 0 || strcmp(weather, "\"main\":\"Drizzle\"") == 0 || strcmp(weather, "\"main\":\"Thunderstorm\"") == 0){

            ESP_LOGI(TAG_CTRL, "\nIt's raining men\r\n");
            
            control_GPIOs(0,0,1); // RGB

        } else if (strcmp(weather, "\"main\":\"Clouds\"") == 0 || strcmp(weather, "\"main\":\"Fog\"") == 0 || strcmp(weather, "\"main\":\"Snow\"") == 0 || strcmp(weather, "\"main\":\"Mist\"") == 0){

            ESP_LOGI(TAG_CTRL, "\nIt's cloudy men\r\n");

            control_GPIOs(1,1,1); // RGB
            

        } else if (strcmp(weather, "\"main\":\"Clear\"") == 0){

            ESP_LOGI(TAG_CTRL, "\nIt's clear men\r\n");

            control_GPIOs(0,1,0); // RGB

        } else {

            ESP_LOGI(TAG_CTRL, "\nIt's wrong men\r\n");

            control_GPIOs(1,0,0); // RGB

        }
        
        // Calculate time till next wake up
        uint32_t time_to_sleep = get_time_to_sleep();

        sleep_seconds(time_to_sleep);

    }
    
}

void sntp_task(void *pvParameter){

    ++boot_count;
    ESP_LOGI(TAG_SNTP, "Boot count: %d", boot_count);

    TaskHandle_t xHandleLed = NULL;

    xTaskCreate(&led_indicator_task, "led_indicator_task", configMINIMAL_STACK_SIZE, (void*)600, 5, &xHandleLed);

    char strftime_buf[64];
    time_t now;
    struct tm timeinfo;

    // time() returns the time since 00:00:00 UTC, January 1, 1970 (Unix timestamp) in seconds. 
    // If now is not a null pointer, the returned value is also stored in the object pointed to by second.
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    ESP_LOGI(TAG_SNTP, "timeinfo.tm_year %d", timeinfo.tm_year);
    
    // tm_year = The number of years since 1900. 
    // Update time if it is not set or if the boot count is higher than ten
    if (timeinfo.tm_year < (2018 - 1900) || boot_count > 10) {
        ESP_LOGI(TAG_SNTP, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
        setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
        tzset();
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG_SNTP, "The current date/time in Madrid is: %s", strftime_buf);
        // Restart device
        sleep_seconds(1);

    }

    uint32_t time_to_sleep;

    printf("TIME --> %d:%d\n", timeinfo.tm_hour, timeinfo.tm_min);

    // If we are on time, or past time, start light control
    if (timeinfo.tm_hour == HOUR_WAKE_UP && timeinfo.tm_min >= MINUTE_WAKE_UP && timeinfo.tm_min < MINUTE_WAKE_UP + TIME_AWAKE){

        vTaskDelete(xHandleLed);

        xTaskCreate(&control_task, "control_task", 8192, NULL, 5, NULL);
        
        vTaskDelete(NULL);

    // If we have woken up early, go back to sleep
    } else {

        time_to_sleep = get_time_to_sleep();

        sleep_seconds(time_to_sleep);
        
    }

}


/* MAIN */
void app_main(){

    configure_GPIOS();

    xTaskCreate(&sntp_task, "sntp_task", 8192, NULL, 5, NULL);

}