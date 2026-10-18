# TensioVigil-IoT

Real-time monitoring of cardiovascular vital signs, with on-device clinical
decision support for hypertensive patients, built on an ESP32 and MQTT.

> **Simulation only.** This project was built and validated in the
> [Wokwi](https://wokwi.com) ESP32 simulator. It has **not** been run on physical
> hardware. Blood pressure and heart rate come from potentiometers standing in
> for real sensors. It is not a medical device.

## What it does

- **Firmware (`firmware/sketch.ino`).** The ESP32 samples systolic and diastolic
  blood pressure, heart rate and body temperature (DHT22). It classifies each
  reading using the 2023 ESH and 2024 ESC hypertension guidance, and shows the
  result on an I²C LCD with tri-colour LEDs. A buzzer sounds only in a
  hypertensive emergency (SBP ≥ 180 or DBP ≥ 120 mmHg).
- **Telemetry.** JSON messages are published over MQTT v3.1.1 to
  `tensiovigil/<patient_id>/vitals` and `.../alert`.
- **Two-way control.** The device subscribes to `.../command`. A clinician
  dashboard can, for example, send `{"cmd":"mute"}` to silence the buzzer
  while the alert record is kept.
- **Dashboards (`dashboard/`).** Browser pages that connect to the broker over
  WebSocket, plot each patient's trends and send commands.

## Status

| Part | Status |
|---|---|
| Firmware logic, classification, MQTT publish/subscribe | Built; validated in the Wokwi simulator |
| Clinician dashboard (browser, MQTT over WebSocket) | Built; tested against the simulator |
| Physical hardware (real cuff, pulse oximeter) | Planned |
| TLS-encrypted, authenticated broker | Planned |
| On-device ML (TinyML arrhythmia detection) | Planned |

## How to run

1. Create a new ESP32 project on wokwi.com.
2. Paste `firmware/sketch.ino` into the sketch, `wokwi/diagram.json` into
   `diagram.json`, and `wokwi/libraries.txt` into the library manager.
3. Start the simulation. It joins the simulator's open `Wokwi-GUEST` network
   and connects to the public test broker `broker.hivemq.com:1883`.
4. Open `dashboard/tensiovigil_dashboard.html` in a browser. Choose the
   "HiveMQ public" preset, connect, and use patient ID `HTN-1010`.

The repository contains no credentials. The firmware uses the simulator's open
guest network and the public, unauthenticated test broker. The dashboard's
username and password fields are empty and are entered at runtime.

## Folder layout

```
tensiovigil-iot/
├── firmware/sketch.ino               # ESP32 firmware (Arduino C++)
├── wokwi/diagram.json                # Wokwi circuit
├── wokwi/libraries.txt               # Arduino libraries used
├── dashboard/index.html              # Live telemetry dashboard
├── dashboard/tensiovigil_dashboard.html  # Clinician dashboard with broker presets
└── README.md
```

## Security review: summary of findings

The project's privacy and security review found these exposures in the prototype:

- **Unencrypted traffic.** Vitals travel over plain MQTT on port 1883 to a
  public broker that anyone can subscribe to. This is acceptable only for a
  simulation using a made-up patient ID. Production needs TLS 1.2 or later on
  port 8883, with mutual X.509 certificate authentication.
- **No broker access control.** Without per-device access lists, one stolen
  credential could publish to another patient's topics. Each device should be
  limited to its own `tensiovigil/<pid>/+` tree.
- **Re-identification risk.** Even a pseudonymous ID combined with vital-sign
  history can identify a person, so GDPR treats it as personal data. Data
  minimisation and a written retention policy are required.
- **Alarm fatigue.** Too many alarms lead clinicians to ignore real ones. The
  buzzer is therefore reserved for emergencies, and the dashboard has an
  acknowledgement workflow.
- **Firmware integrity.** A malicious over-the-air update could turn the
  monitor into a data-exfiltration device. Signed updates and secure boot are
  required before any device leaves the lab.
- **Dashboard back-end.** Recent healthcare-IoT reviews show SQL injection is
  still a leading attack vector. Any future back-end must use parameterised
  queries and input validation.

None of these hardening steps are implemented yet. They are listed above as Planned.

## Notes

- Developed with AI assistance (Claude).

## Licence

All rights reserved.
