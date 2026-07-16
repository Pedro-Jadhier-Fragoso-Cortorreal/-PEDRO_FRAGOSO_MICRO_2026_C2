#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "KAKATA-433";

// --- MAPEADO DE PINES SEGÚN TU ESQUEMÁTICO ---
#define LED1 GPIO_NUM_45[cite: 1]
#define LED2 GPIO_NUM_48[cite: 1]
#define LED3 GPIO_NUM_47[cite: 1]
#define LED4 GPIO_NUM_21[cite: 1]
#define LED5 GPIO_NUM_14[cite: 1]
#define LED6 GPIO_NUM_13[cite: 1]

// Botones de frente y gatillos
#define BTN_GAT_L1   GPIO_NUM_4  // BTN_JOY1[cite: 1]
#define BTN_GAT_R1   GPIO_NUM_5  // BTN_JOY2[cite: 1]
#define BTN_FRENTE_0 GPIO_NUM_6  // BTN0[cite: 1]
#define BTN_FRENTE_1 GPIO_NUM_7  // BTN1[cite: 1]
#define BTN_FRENTE_2 GPIO_NUM_15 // BTN2[cite: 1]
#define BTN_FRENTE_3 GPIO_NUM_16 // BTN3[cite: 1]
// Botones adicionales laterales / gatillos traseros (Mapeados desde el esquemático)[cite: 1]
#define BTN_GAT_L2   GPIO_NUM_38 // BTNL1[cite: 1]
#define BTN_GAT_R2   GPIO_NUM_39 // BTNL2[cite: 1]

// Joysticks (ADC1)
#define JOY_X_CHANNEL ADC_CHANNEL_0 // GPIO 1[cite: 1]
#define JOY_Y_CHANNEL ADC_CHANNEL_1 // GPIO 2[cite: 1]

// I2C para el MPU-6050 y la Pantalla Gráfica[cite: 1]
#define I2C_MASTER_SDA_IO GPIO_NUM_8   // I2C_SDA[cite: 1]
#define I2C_MASTER_SCL_IO GPIO_NUM_9   // I2C_SCL[cite: 1]
#define I2C_MASTER_FREQ_HZ 400000
#define I2C_MASTER_PORT I2C_NUM_0

#define MPU6050_ADDR 0x68

// --- PARÁMETROS DE CONFIGURACIÓN DE RED ---
#define WIFI_SSID      "TU_SSID_WIFI"
#define WIFI_PASS      "TU_CONTRASEÑA"
#define MQTT_BROKER_UR "mqtt://broker.hivemq.com" // Servidor público para ver en el celular
#define MQTT_TOPIC     "kakata433/control/datos"

// --- VARIABLES GLOBALES Y ESTRUCTURAS ---
adc_oneshot_unit_handle_t adc1_handle;
esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_conectado = false;

// Estructura de datos del KAKATA-433
typedef struct {
    int joy_x;         // -100 a +100
    int joy_y;         // -100 a +100
    int gyro_x;        // -100 a +100 (Inclinación mapeada)
    int gyro_y;        // -100 a +100 (Inclinación mapeada)
    float accel_x;     // Gs reales
    float accel_y;     // Gs reales
    float accel_z;     // Gs reales
    bool gatillos[4];  // L1, R1, L2, R2
    bool botones[4];   // F0, F1, F2, F3
} kakata_data_t;

kakata_data_t datos_control;

// Offsets de calibración para Joysticks (Punto cero)
int joy_x_offset = 2048;
int joy_y_offset = 2048;

// --- PROTOTIPOS ---
void init_hardware(void);
void init_i2c(void);
void init_mpu6050(void);
void leer_mpu6050(int16_t *ax, int16_t *ay, int16_t *az, int16_t *gx, int16_t *gy);
void init_wifi(void);
void init_mqtt(void);
void guardar_calibracion(int x, int y);
void cargar_calibracion(void);

// --- MAPEO DE RANGOS (-100 a 100) ---
int mapear_joystick(int raw, int offset) {
    int val = raw - offset;
    // Si el offset desplaza el centro, escalamos simétricamente
    if (val > 0) {
        return (val * 100) / (4095 - offset);
    } else {
        return (val * 100) / offset;
    }
}

// --- MANEJADOR DE CALIBRACIÓN POR HARDWARE ---
// Al presionar los dos botones gatillo de arriba (L1 y R1) por 3 segundos
void evaluar_calibracion(void) {
    static uint32_t tiempo_presionado = 0;
    
    // Recuerda que los botones con pull-up son activos en bajo (0 = Presionado)[cite: 1]
    if (gpio_get_level(BTN_GAT_L1) == 0 && gpio_get_level(BTN_GAT_R1) == 0) {
        tiempo_presionado += 100; // Incremento de ciclo de 100ms
        
        if (tiempo_presionado >= 3000) { // 3 segundos continuos
            ESP_LOGI(TAG, "¡CALIBRANDO JOYSTICKS! No los muevas...");
            
            int acum_x = 0, acum_y = 0;
            for (int i = 0; i < 10; i++) {
                int rx, ry;
                adc_oneshot_read(adc1_handle, JOY_X_CHANNEL, &rx);
                adc_oneshot_read(adc1_handle, JOY_Y_CHANNEL, &ry);
                acum_x += rx;
                acum_y += ry;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            joy_x_offset = acum_x / 10;
            joy_y_offset = acum_y / 10;
            
            guardar_calibracion(joy_x_offset, joy_y_offset);
            ESP_LOGI(TAG, "Calibración exitosa. Nuevo Centro: X=%d, Y=%d", joy_x_offset, joy_y_offset);
            
            // Parpadeo rápido de LEDs como confirmación visual
            for (int i=0; i<3; i++) {
                gpio_set_level(LED1, 0); vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED1, 1); vTaskDelay(pdMS_TO_TICKS(100));
            }
            tiempo_presionado = 0;
        }
    } else {
        tiempo_presionado = 0;
    }
}

// --- COMUNICACIÓN MQTT ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado al servidor MQTT broker");
            mqtt_conectado = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Desconectado del servidor MQTT");
            mqtt_conectado = false;
            break;
        default:
            break;
    }
}

// --- TAREA DE PANTALLA GRÁFICA (DISPLAY SIMULATION) ---
void tarea_pantalla(void *pvParameters) {
    while (1) {
        // En esta sección se actualizan los buffers de dibujo de la pantalla (SSD1306/I2C)
        // Ejemplo de visualización serializada del diseño de la interfaz gráfica del KAKATA-433:
        printf("\e[1;1H\e[2J"); // Limpiar terminal para simulación de refresco gráfico
        printf("=========================================\n");
        printf("       PANTALLA KAKATA-433 REMOTE        \n");
        printf("=========================================\n");
        printf(" Wi-Fi: %s  |  MQTT: %s\n", 
               (esp_wifi_get_mode(NULL) == ESP_OK) ? "OK" : "DISCON", 
               mqtt_conectado ? "ONLINE" : "OFFLINE");
        printf("-----------------------------------------\n");
        printf(" JOYSTICK CH  ->  X: %4d  |  Y: %4d\n", datos_control.joy_x, datos_control.joy_y);
        printf(" GIROSCOPIO   ->  X: %4d  |  Y: %4d\n", datos_control.gyro_x, datos_control.gyro_y);
        printf(" ACELERÓMETRO ->  X: %.2f G |  Y: %.2f G | Z: %.2f G\n", 
               datos_control.accel_x, datos_control.accel_y, datos_control.accel_z);
        printf("-----------------------------------------\n");
        printf(" GATILLOS: L1:[%c] R1:[%c] | L2:[%c] R2:[%c]\n", 
               datos_control.gatillos[0]?'X':' ', datos_control.gatillos[1]?'X':' ',
               datos_control.gatillos[2]?'X':' ', datos_control.gatillos[3]?'X':' ');
        printf(" BOTONES:  F0:[%c] F1:[%c] F2:[%c] F3:[%c]\n", 
               datos_control.botones[0]?'X':' ', datos_control.botones[1]?'X':' ',
               datos_control.botones[2]?'X':' ', datos_control.botones[3]?'X':' ');
        printf("=========================================\n");
        
        vTaskDelay(pdMS_TO_TICKS(200)); // Refresco de pantalla de 5 FPS
    }
}

// --- PROGRAMA PRINCIPAL ---
void app_main(void)
{
    // Carga calibración guardada en la Flash
    cargar_calibracion();

    // Inicializar Periféricos e Interfaces
    init_hardware();
    init_i2c();
    init_mpu6050();
    init_wifi();
    init_mqtt();

    // Crear la tarea para procesar la interfaz gráfica
    xTaskCreate(tarea_pantalla, "tarea_pantalla", 4096, NULL, 5, NULL);

    char payload[256];

    while(1)
    {
        // 1. Validar proceso de calibración automática
        evaluar_calibracion();

        // 2. Leer Botones (Lógica invertida debido a resistencias Pull-Up)[cite: 1]
        datos_control.gatillos[0] = !gpio_get_level(BTN_GAT_L1);
        datos_control.gatillos[1] = !gpio_get_level(BTN_GAT_R1);
        datos_control.gatillos[2] = !gpio_get_level(BTN_GAT_L2);
        datos_control.gatillos[3] = !gpio_get_level(BTN_GAT_R2);

        datos_control.botones[0]  = !gpio_get_level(BTN_FRENTE_0);
        datos_control.botones[1]  = !gpio_get_level(BTN_FRENTE_1);
        datos_control.botones[2]  = !gpio_get_level(BTN_FRENTE_2);
        datos_control.botones[3]  = !gpio_get_level(BTN_FRENTE_3);

        // 3. Encender los LEDs físicos según el estado de los botones[cite: 1]
        gpio_set_level(LED1, datos_control.gatillos[0]);
        gpio_set_level(LED2, datos_control.gatillos[1]);
        gpio_set_level(LED3, datos_control.botones[0]);
        gpio_set_level(LED4, datos_control.botones[1]);

        // 4. Leer Joysticks (ADC) y mapear a rango [-100, 100]
        int raw_x, raw_y;
        adc_oneshot_read(adc1_handle, JOY_X_CHANNEL, &raw_x);
        adc_oneshot_read(adc1_handle, JOY_Y_CHANNEL, &raw_y);
        datos_control.joy_x = mapear_joystick(raw_x, joy_x_offset);
        datos_control.joy_y = mapear_joystick(raw_y, joy_y_offset);

        // 5. Leer Giroscopio / Acelerómetro MPU-6050[cite: 1]
        int16_t ax, ay, az, gx, gy;
        leer_mpu6050(&ax, &ay, &az, &gx, &gy);

        // Convertir acelerómetro a Gs reales (escala por defecto +-2g: dividir por 16384)
        datos_control.accel_x = (float)ax / 16384.0;
        datos_control.accel_y = (float)ay / 16384.0;
        datos_control.accel_z = (float)az / 16384.0;

        // Mapear Giroscopio a rango [-100, 100] (escala por defecto +-250°/s)
        datos_control.gyro_x = (int)(gx / 327);
        datos_control.gyro_y = (int)(gy / 327);

        // Truncar límites por seguridad matemática
        if(datos_control.gyro_x > 100)  datos_control.gyro_x = 100;
        if(datos_control.gyro_x < -100) datos_control.gyro_x = -100;
        if(datos_control.gyro_y > 100)  datos_control.gyro_y = 100;
        if(datos_control.gyro_y < -100) datos_control.gyro_y = -100;

        // 6. Transmisión de datos por MQTT (JSON para visualizar en App celular)
        if (mqtt_conectado) {
            snprintf(payload, sizeof(payload), 
                "{\"joy_x\":%d,\"joy_y\":%d,\"gyro_x\":%d,\"gyro_y\":%d,"
                "\"accel\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
                "\"gatillos\":[%d,%d,%d,%d],\"botones\":[%d,%d,%d,%d]}",
                datos_control.joy_x, datos_control.joy_y,
                datos_control.gyro_x, datos_control.gyro_y,
                datos_control.accel_x, datos_control.accel_y, datos_control.accel_z,
                datos_control.gatillos[0], datos_control.gatillos[1], datos_control.gatillos[2], datos_control.gatillos[3],
                datos_control.botones[0], datos_control.botones[1], datos_control.botones[2], datos_control.botones[3]);

            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Actualización de datos a 10Hz
    }
}

// --- DETALLES DE IMPLEMENTACIÓN DE BAJO NIVEL (I2C / MPU-6050 / WIFI) ---

void init_hardware(void) {
    configurar_led(LED1); configurar_led(LED2);
    configurar_led(LED3); configurar_led(LED4);
    configurar_led(LED5); configurar_led(LED6);

    configurar_boton(BTN_GAT_L1); configurar_boton(BTN_GAT_R1);
    configurar_boton(BTN_GAT_L2); configurar_boton(BTN_GAT_R2);
    configurar_boton(BTN_FRENTE_0); configurar_boton(BTN_FRENTE_1);
    configurar_boton(BTN_FRENTE_2); configurar_boton(BTN_FRENTE_3);

    // ADC setup
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, JOY_X_CHANNEL, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, JOY_Y_CHANNEL, &config));
}

void init_i2c(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_PORT, &conf);
    i2c_driver_install(I2C_MASTER_PORT, conf.mode, 0, 0, 0);
}

void init_mpu6050(void) {
    // Despertar el MPU-6050 escribiendo '0' en el registro de energía (0x6B)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x6B, true);
    i2c_master_write_byte(cmd, 0, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
}

void leer_mpu6050(int16_t *ax, int16_t *ay, int16_t *az, int16_t *gx, int16_t *gy) {
    uint8_t data[14];
    // Apuntar al registro de inicio de datos (0x3B, ACCEL_XOUT_H)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x3B, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 14, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    *ax = (data[0] << 8) | data[1];
    *ay = (data[2] << 8) | data[3];
    *az = (data[4] << 8) | data[5];
    *gx = (data[8] << 8) | data[9];
    *gy = (data[10] << 8) | data[11];
}

// --- CONECTIVIDAD WI-FI ---
void init_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
}

// --- CONECTIVIDAD MQTT ---
void init_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_UR,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// --- PERSISTENCIA NVS PARA CALIBRACIÓN ---
void guardar_calibracion(int x, int y) {
    nvs_handle_t mi_nvs;
    if (nvs_open("almacen", NVS_READWRITE, &mi_nvs) == ESP_OK) {
        nvs_set_i32(mi_nvs, "offset_x", x);
        nvs_set_i32(mi_nvs, "offset_y", y);
        nvs_commit(mi_nvs);
        nvs_close(mi_nvs);
    }
}

void cargar_calibracion(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    nvs_handle_t mi_nvs;
    if (nvs_open("almacen", NVS_READONLY, &mi_nvs) == ESP_OK) {
        nvs_get_i32(mi_nvs, "offset_x", &joy_x_offset);
        nvs_get_i32(mi_nvs, "offset_y", &joy_y_offset);
        nvs_close(mi_nvs);
    }
}