# Fronius-Display

Eigenständiges Touch-Display für **Fronius-Wechselrichter** (getestet: Symo GEN24) –
auf einem günstigen ESP32-S3-Display, ohne ioBroker, Home Assistant oder Server.
Das Display holt sich die Werte per WLAN direkt vom Wechselrichter (Fronius Solar API).

> **Kein offizielles Produkt von Fronius.** Bei Fragen zum Display bitte hier im
> Repository ein Issue anlegen, nicht bei Fronius. Nutzung auf eigene Gefahr.

> **Status: in Entwicklung** – noch keine Firmware zum Installieren.

## Vorschau

![Energiefluss](docs/images/vorschau-energiefluss.png)

*Entwurf der Startseite (gerendert, Beispielwerte)*

## Geplante Funktionen

- **Energiefluss** wie in Solar.web: PV, Verbraucher, Netz und Batterie als Segmentringe
  um den Wechselrichter, laufende Punkte zeigen die Flussrichtung
- **Antippen öffnet Details:**
  - PV – Aufteilung je Wechselrichter (z. B. GEN24 + zusätzlicher Wechselrichter
    über Fronius Smart Meter), später Strings über Modbus
  - Netz – Phasen L1–L3 mit Spannung, Strom, Leistung, Frequenz
  - Batterie – Ladestand, Lade-/Entladeleistung, „reicht noch ca. …“ / „voll in ca. …“
  - Verbraucher – Haus, später Wallbox (Wattpilot) und Heizstab (Ohmpilot) getrennt
- **Tageswerte:** PV-Ertrag, Verbrauch, Netzbezug, Einspeisung, Batterie geladen,
  Autarkie, Eigenverbrauch – aus den Zählerständen selbst berechnet
  (der GEN24 liefert keine Tageswerte)
- **Statistik** für Tag, Monat, Jahr; optional mit **SD-Karte** auch Tagesverläufe
- **Ausrichtung wählbar:** USB-Anschluss links oder rechts (Drehung um 180°)
- **Einrichtung komplett am Display:** WLAN, feste IP oder DHCP, IP des Wechselrichters,
  Ringskalierung (kWp), Batteriekapazität
- **Updates über WLAN** und Installation im Browser – wie beim
  [Violet Pool-Display](https://github.com/AlPi-BRV/violet-display)

## Voraussetzungen am Wechselrichter

Im Webinterface des Wechselrichters muss die **Solar API** eingeschaltet sein
(Kommunikation → Solar API). Test im Browser:

```
http://<IP-des-Wechselrichters>/solar_api/v1/GetPowerFlowRealtimeData.fcgi
```

Erscheint dort JSON, ist alles bereit.

## Hardware

| | |
|---|---|
| Display | **Elecrow CrowPanel ESP32 5.0"**, SKU **DIS07050H**, Board-Revision **V3.0**<br>oder **Elecrow CrowPanel ESP32 7.0"**, SKU **DIS08070H**, Board-Revision **V3.0** |
| Chip | ESP32-S3-WROOM-1-N4R8 (4 MB Flash, 8 MB PSRAM) |
| Auflösung | 800 × 480, kapazitiver Touch |

Beide Größen brauchen **jeweils eigene Firmware** – jede Datei trägt die Größe im Namen
(`fronius-display-5zoll-…` / `fronius-display-7zoll-…`).

## Lizenz

MIT – siehe [LICENSE](LICENSE).
Verwendete Bibliotheken: [LovyanGFX](https://github.com/lovyan03/LovyanGFX),
[ArduinoJson](https://github.com/bblanchon/ArduinoJson).
Schrift: [Inter](https://rsms.me/inter/) (SIL Open Font License).
