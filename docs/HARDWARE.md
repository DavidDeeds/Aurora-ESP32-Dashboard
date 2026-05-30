# Hardware Notes

Target board: Waveshare ESP32-S3-RS485-CAN.

## Wiring Table

| Board terminal | Inverter terminal | Notes |
| --- | --- | --- |
| RS485 A / D+ | +T/R or RS485+ | Confirm inverter label before energizing. |
| RS485 B / D- | -T/R or RS485- | Swap A/B if all polls fail. |
| GND / RTN | RTN / signal ground | Follow inverter manual grounding guidance. |
| 7-36 V input | DC supply | Do not power from USB and terminal supply at the same time. |

## Bring-up

1. Flash firmware from this repository (see root `README.md`).
2. Connect RS-485 A/B and ground per the table above.
3. Use the web UI to set inverter address and verify polling on the dashboard.
