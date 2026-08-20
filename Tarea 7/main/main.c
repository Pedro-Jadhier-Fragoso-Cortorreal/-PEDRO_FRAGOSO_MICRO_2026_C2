#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "sdkconfig.h"

#define PB1_GPIO             CONFIG_REACCION_PB1_GPIO
#define PB2_GPIO             CONFIG_REACCION_PB2_GPIO
#define LED_GPIO             CONFIG_REACCION_LED_GPIO
#define DEBOUNCE_US          2000
#define PB2_PRESS_TIMEOUT_US 10000000LL
#define PB2_HOLD_TIMEOUT_US  60000000LL
#define DELAY_MIN_MS         CONFIG_REACCION_DELAY_MIN_MS
#define DELAY_MAX_MS         CONFIG_REACCION_DELAY_MAX_MS
#define DELAY_RANGE_MS       (DELAY_MAX_MS > DELAY_MIN_MS ? (uint32_t)(DELAY_MAX_MS - DELAY_MIN_MS) : 0)

static const char *TAG = "reaccion";

typedef enum {
    ST_IDLE,
    ST_ARMED,
    ST_SIGNALED,
    ST_WAIT_PB2_PRESS,
    ST_WAIT_PB2_HOLD
} state_t;

typedef struct {
    gpio_num_t gpio;
    bool pressed;
    int64_t us;
} btn_event_t;

static QueueHandle_t s_btn_queue = NULL;
static esp_mqtt_client_handle_t mqtt = NULL;
static volatile bool g_reset = false;
static uint32_t g_trial = 0;

static void mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (mqtt == NULL) {
        return;
    }
    esp_mqtt_client_publish(mqtt, topic, payload, 0, qos, retain);
}

static void publish_state(const char *st)
{
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/state", CONFIG_REACCION_MQTT_TOPIC_PREFIX);
    mqtt_publish(topic, st, 1, true);
}

static void publish_result(int64_t reaction_us, int64_t hold_us, int64_t delay_us)
{
    g_trial++;
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/result", CONFIG_REACCION_MQTT_TOPIC_PREFIX);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "reaction_ms", reaction_us / 1000.0);
    cJSON_AddNumberToObject(j, "hold_ms", hold_us / 1000.0);
    cJSON_AddNumberToObject(j, "delay_ms", delay_us / 1000.0);
    cJSON_AddNumberToObject(j, "trial", g_trial);
    cJSON_AddNumberToObject(j, "uptime_ms", (double)esp_timer_get_time() / 1000.0);

    char *s = cJSON_PrintUnformatted(j);
    if (s != NULL) {
        mqtt_publish(topic, s, 1, true);
        ESP_LOGI(TAG, "resultado: %s", s);
        cJSON_free(s);
    }
    cJSON_Delete(j);
}

static void IRAM_ATTR button_isr(void *arg)
{
    gpio_num_t gpio = (gpio_num_t)(intptr_t)arg;
    btn_event_t ev = {
        .gpio = gpio,
        .pressed = gpio_get_level(gpio) == 0,
        .us = esp_timer_get_time(),
    };
    xQueueSendFromISR(s_btn_queue, &ev, NULL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    char topic[64];

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado");
        snprintf(topic, sizeof(topic), "%s/status", CONFIG_REACCION_MQTT_TOPIC_PREFIX);
        mqtt_publish(topic, "online", 1, true);
        snprintf(topic, sizeof(topic), "%s/command", CONFIG_REACCION_MQTT_TOPIC_PREFIX);
        esp_mqtt_client_subscribe(mqtt, topic, 1);
        publish_state("IDLE");
        break;

    case MQTT_EVENT_DATA:
        snprintf(topic, sizeof(topic), "%s/command", CONFIG_REACCION_MQTT_TOPIC_PREFIX);
        if (event->topic_len == (int)strlen(topic) &&
            memcmp(event->topic, topic, event->topic_len) == 0) {
            if (event->data_len == 5 && memcmp(event->data, "reset", 5) == 0) {
                g_reset = true;
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado");
        break;

    default:
        break;
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_netif_create_default_wifi_sta();

    wifi_config_t wc = {
        .sta = {
            .ssid = CONFIG_REACCION_WIFI_SSID,
            .password = CONFIG_REACCION_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void wifi_mqtt_task(void *arg)
{
    static char will_topic[64];

    wifi_init();

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA");
    while (sta == NULL || !esp_netif_is_netif_up(sta)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi conectado");

    snprintf(will_topic, sizeof(will_topic), "%s/status", CONFIG_REACCION_MQTT_TOPIC_PREFIX);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_REACCION_MQTT_URI,
        .session.last_will.topic = will_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
    if (strlen(CONFIG_REACCION_MQTT_USER) > 0) {
        mqtt_cfg.credentials.username = CONFIG_REACCION_MQTT_USER;
        mqtt_cfg.credentials.authentication.password = CONFIG_REACCION_MQTT_PASSWORD;
    }

    mqtt = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void reaction_task(void *arg)
{
    bool pb1_stable = true;
    bool pb2_stable = true;
    int64_t pb1_last_us = 0;
    int64_t pb2_last_us = 0;
    state_t st = ST_IDLE;
    int64_t t_press1 = 0;
    int64_t t_led = 0;
    int64_t t_release = 0;
    int64_t t_pb2_press = 0;
    uint32_t delay_ms = DELAY_MIN_MS;

    gpio_set_level(LED_GPIO, 0);
    publish_state("IDLE");

    for (;;) {
        btn_event_t ev;
        while (xQueueReceive(s_btn_queue, &ev, pdMS_TO_TICKS(10))) {
            bool is_pb1 = (ev.gpio == PB1_GPIO);
            int64_t *last_us = is_pb1 ? &pb1_last_us : &pb2_last_us;
            bool *stable = is_pb1 ? &pb1_stable : &pb2_stable;

            bool fresh = (ev.us - *last_us >= DEBOUNCE_US) && (ev.pressed != *stable);
            *last_us = ev.us;
            if (!fresh) {
                continue;
            }
            *stable = ev.pressed;

            if (is_pb1) {
                switch (st) {
                case ST_IDLE:
                    if (ev.pressed) {
                        st = ST_ARMED;
                        t_press1 = ev.us;
                        delay_ms = DELAY_MIN_MS +
                            (DELAY_RANGE_MS > 0 ? (uint32_t)(esp_random() % (DELAY_RANGE_MS + 1)) : 0);
                        publish_state("ARMED");
                    }
                    break;

                case ST_ARMED:
                    if (!ev.pressed) {
                        st = ST_IDLE;
                        publish_state("IDLE");
                    }
                    break;

                case ST_SIGNALED:
                    if (!ev.pressed) {
                        t_release = ev.us;
                        gpio_set_level(LED_GPIO, 0);
                        st = ST_WAIT_PB2_PRESS;
                        publish_state("PB2");
                    }
                    break;

                default:
                    break;
                }
            } else {
                switch (st) {
                case ST_WAIT_PB2_PRESS:
                    if (ev.pressed) {
                        t_pb2_press = ev.us;
                        st = ST_WAIT_PB2_HOLD;
                    }
                    break;

                case ST_WAIT_PB2_HOLD:
                    if (!ev.pressed) {
                        publish_result(t_release - t_led, ev.us - t_pb2_press,
                                       (int64_t)delay_ms * 1000);
                        st = ST_IDLE;
                        publish_state("IDLE");
                    }
                    break;

                default:
                    break;
                }
            }
        }

        if (g_reset) {
            g_reset = false;
            st = ST_IDLE;
            gpio_set_level(LED_GPIO, 0);
            publish_state("IDLE");
        }

        int64_t now = esp_timer_get_time();

        if (st == ST_ARMED) {
            if (now - t_press1 >= (int64_t)delay_ms * 1000) {
                gpio_set_level(LED_GPIO, 1);
                t_led = esp_timer_get_time();
                st = ST_SIGNALED;
                publish_state("SIGNALED");
            }
        } else if (st == ST_WAIT_PB2_PRESS) {
            if (now - t_release > PB2_PRESS_TIMEOUT_US) {
                st = ST_IDLE;
                publish_state("IDLE");
            }
        } else if (st == ST_WAIT_PB2_HOLD) {
            if (now - t_pb2_press > PB2_HOLD_TIMEOUT_US) {
                publish_result(t_release - t_led, PB2_HOLD_TIMEOUT_US, (int64_t)delay_ms * 1000);
                st = ST_IDLE;
                publish_state("IDLE");
            }
        }
    }
}

void app_main(void)
{
    gpio_config_t btn_io = {
        .pin_bit_mask = (1ULL << PB1_GPIO) | (1ULL << PB2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&btn_io);

    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io);
    gpio_set_level(LED_GPIO, 0);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_btn_queue = xQueueCreate(16, sizeof(btn_event_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PB1_GPIO, button_isr, (void *)(intptr_t)PB1_GPIO);
    gpio_isr_handler_add(PB2_GPIO, button_isr, (void *)(intptr_t)PB2_GPIO);

#if CONFIG_FREERTOS_UNICORE
    xTaskCreate(wifi_mqtt_task, "mqtt", 4096, NULL, 4, NULL);
    xTaskCreate(reaction_task, "reaction", 4096, NULL, 5, NULL);
#else
    xTaskCreatePinnedToCore(wifi_mqtt_task, "mqtt", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(reaction_task, "reaction", 4096, NULL, 6, NULL, 1);
#endif
}
