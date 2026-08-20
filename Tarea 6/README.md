# Rhumana - Medidor de reaccion humana

Sistema para medir la reaccion humana ante un estimulo visual usando un **ESP32** (original, ESP32-C3 o ESP32-S3).

## Como funciona

1. El usuario **presiona PB1 y lo sostiene**, esperando la senal.
2. El micro espera un tiempo **aleatorio** (configurable, por defecto 1500–4000 ms) y **enciende el LED**.
3. El usuario **suelta PB1** lo mas rapido posible: eso es el **tiempo de reaccion**.
4. Inmediatamente el usuario **presiona PB2 y lo mantiene**; el tiempo que lo mantiene es el **tiempo de sostenimiento** (segunda variable medida).
5. Al soltar PB2, el micro publica el resultado por **MQTT** y queda listo para la siguiente prueba.

Todo se mide con **interrupciones de GPIO** (la marca de tiempo se captura dentro del ISR con `esp_timer_get_time()` en microsegundos) y se reporta en **milisegundos con 1 decimal** (precision < 1 ms).

El codigo funciona tanto en ESP32-C3 (un nucleo) como en ESP32-S3 (dos nucleos) y en un solo nucleo no bloquea al WiFi/MQTT porque la tarea de medicion espera eventos en una cola en vez de hacer *busy-polling*.

## Estructura

```
Rhumana/
├── main/                 # firmware ESP-IDF (ESP32)
│   ├── main.c            # logica de medicion (ISR + cola) + WiFi + MQTT
│   ├── CMakeLists.txt
│   └── Kconfig.projbuild # pines, WiFi y broker configurables
├── pc_logger/            # registro de mediciones en el PC
│   ├── mqtt_logger.py
│   └── requirements.txt
└── docs/
    ├── cableado.md       # conexiones del hardware
    └── panel_celular.md  # configuracion de la app del celular
```

## Configuracion (menuconfig)

```bash
idf.py menuconfig
```

En **Reaction Timer Configuration**:

| Opcion | Valor por defecto | Descripcion |
|---|---|---|
| `RHUMANA_PB1_GPIO` | 4 | GPIO del boton PB1 |
| `RHUMANA_PB2_GPIO` | 5 | GPIO del boton PB2 |
| `RHUMANA_LED_GPIO` | 13 | GPIO del LED (en ESP32 evita GPIO6-11, son de la flash) |
| `RHUMANA_DELAY_MIN_MS` | 1500 | retardo aleatorio minimo |
| `RHUMANA_DELAY_MAX_MS` | 4000 | retardo aleatorio maximo |
| `RHUMANA_WIFI_SSID` | myssid | red WiFi |
| `RHUMANA_WIFI_PASSWORD` | (vacio) | clave WiFi |
| `RHUMANA_MQTT_URI` | mqtt://broker.hivemq.com:1883 | broker publico |
| `RHUMANA_MQTT_TOPIC_PREFIX` | rhumana | prefijo de topics |

## Compilar y flashear

```bash
# En el terminal de ESP-IDF (PowerShell)
cd C:\Users\Acer Aspire 11TH\workspace\Rhumana
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor
```

> **Importante para flashear:** indica siempre el puerto con `-p`. Sin `-p`,
> `idf.py flash` no encuentra el dispositivo y falla con `ninja failed...
> esptool ... failed`. Si tu PC usa otro numero de COM, revisalo en el
> Administrador de dispositivos (Puertos COM y LPT).

## Topics MQTT

| Topic | Retenido | Contenido |
|---|---|---|
| `rhumana/state` | si | fase actual: `IDLE`, `ARMED`, `SIGNALED`, `PB2` |
| `rhumana/result` | si | resultado JSON de la ultima prueba |
| `rhumana/status` | si (LWT) | `online` / `offline` |
| `rhumana/command` | no | comando recibido (`reset` aborta la prueba) |

Ejemplo de `rhumana/result`:

```json
{"reaction_ms":245.3,"hold_ms":312.7,"delay_ms":2500,"trial":7,"uptime_ms":61234.5}
```

- `reaction_ms`: tiempo desde que se enciende el LED hasta que se suelta PB1.
- `hold_ms`: tiempo que se mantiene presionado PB2.
- `delay_ms`: retardo aleatorio usado en esa prueba.
- `trial`: numero de prueba desde el arranque.
- `uptime_ms`: tiempo de funcionamiento del micro.

## Ver en el celular

Usa una app MQTT como **MQTT Dash** o **IoT MQTT Panel**: ver [docs/panel_celular.md](docs/panel_celular.md).

## Registro en el PC

```bash
pip install -r pc_logger/requirements.txt
python pc_logger/mqtt_logger.py
```

Genera un CSV con cada medicion. Ver comentarios en `pc_logger/mqtt_logger.py`.
