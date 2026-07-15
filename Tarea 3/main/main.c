#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_2
#define BUTTON_GPIO GPIO_NUM_0

typedef enum
{
    ESTADO_LED_APAGADO = 0,
    ESTADO_LED_ENCENDIDO

} estado_t;

estado_t estado_actual = ESTADO_LED_APAGADO;

void configurar_led(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
}

void configurar_boton(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
}

void maquina_estados(void)
{
    switch(estado_actual)
    {
        case ESTADO_LED_APAGADO:

            gpio_set_level(LED_GPIO, 0);

            if(gpio_get_level(BUTTON_GPIO) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(200));
                estado_actual = ESTADO_LED_ENCENDIDO;
            }

            break;

        case ESTADO_LED_ENCENDIDO:

            gpio_set_level(LED_GPIO, 1);

            if(gpio_get_level(BUTTON_GPIO) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(200));
                estado_actual = ESTADO_LED_APAGADO;
            }

            break;
    }
}

void app_main(void)
{
    configurar_led();
    configurar_boton();

    while(1)
    {
        maquina_estados();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}