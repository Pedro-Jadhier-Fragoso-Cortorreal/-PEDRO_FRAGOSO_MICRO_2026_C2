#!/usr/bin/env python3
"""Suscripcion a rhumana/result y rhumana/state y registro de mediciones en CSV.

Uso:
    pip install -r requirements.txt
    python mqtt_logger.py [--broker broker.hivemq.com] [--port 1883] [--prefix rhumana] [--csv rhumana_log.csv]
"""

import argparse
import csv
import json
import sys
import time

import paho.mqtt.client as mqtt

client_id = "rhumana-logger-%d" % int(time.time())
last_trial = {}


def make_client():
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:
        return mqtt.Client(client_id=client_id)


def append_row(csv_path, row, fieldnames):
    new_file = not __import__("os").path.exists(csv_path)
    with open(csv_path, "a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if new_file:
            writer.writeheader()
        writer.writerow(row)


def on_connect(client, userdata, flags, *args):
    rc = args[0] if args else 0
    print("Conectado al broker (rc=%s)" % rc)
    prefix = userdata["prefix"]
    client.subscribe("%s/result" % prefix, qos=1)
    client.subscribe("%s/state" % prefix, qos=1)


def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode("utf-8", errors="replace")
    prefix = userdata["prefix"]
    csv_path = userdata["csv"]
    now = time.strftime("%Y-%m-%d %H:%M:%S")

    if topic.endswith("/result"):
        try:
            d = json.loads(payload)
        except json.JSONDecodeError:
            print("[%s] %s" % (now, payload))
            return

        trial = d.get("trial")
        if trial is not None and last_trial.get("trial") == trial and last_trial.get("key") == topic:
            return
        if trial is not None:
            last_trial["trial"] = trial
            last_trial["key"] = topic

        row = {
            "fecha": now,
            "trial": trial,
            "reaction_ms": d.get("reaction_ms"),
            "hold_ms": d.get("hold_ms"),
            "delay_ms": d.get("delay_ms"),
            "uptime_ms": d.get("uptime_ms"),
        }
        append_row(csv_path, row, list(row.keys()))
        print("[%s] trial=%s reaccion=%.1f ms sostener=%.1f ms delay=%.0f ms  -> %s" % (
            now, trial, row["reaction_ms"] or 0, row["hold_ms"] or 0,
            row["delay_ms"] or 0, csv_path))
    elif topic.endswith("/state"):
        print("[%s] estado: %s" % (now, payload))


def main():
    parser = argparse.ArgumentParser(description="Registro de reacciones humanas via MQTT")
    parser.add_argument("--broker", default="broker.hivemq.com", help="Direccion del broker MQTT")
    parser.add_argument("--port", type=int, default=1883, help="Puerto del broker")
    parser.add_argument("--prefix", default="rhumana", help="Prefijo de topics")
    parser.add_argument("--csv", default="rhumana_log.csv", help="Archivo CSV de salida")
    args = parser.parse_args()

    client = make_client()
    client.user_data_set({"prefix": args.prefix, "csv": args.csv})
    client.on_connect = on_connect
    client.on_message = on_message

    print("Suscribiendose a %s/result y %s/state en %s:%d" % (
        args.prefix, args.prefix, args.broker, args.port))
    try:
        client.connect(args.broker, args.port, 60)
    except Exception as exc:
        print("No se pudo conectar al broker: %s" % exc)
        sys.exit(1)

    client.loop_forever()


if __name__ == "__main__":
    main()
