# Fronius-Display

Eigenständiges Touch-Display für **Fronius-Wechselrichter** (getestet: Symo GEN24) –
auf einem günstigen ESP32-S3-Display, ohne ioBroker, Home Assistant oder Server.
Das Display holt sich die Werte per WLAN direkt vom Wechselrichter (Fronius Solar API).

> **Kein offizielles Produkt von Fronius.** Bei Fragen zum Display bitte hier im
> Repository ein Issue anlegen, nicht bei Fronius. Nutzung auf eigene Gefahr.

> **Status: 0.1.0 in Entwicklung** – die Firmware baut für 5" und 7", ist aber noch nicht
> auf einem Display getestet. Noch kein Release, daher auch noch kein Web-Installer.

## Vorschau

![Energiefluss](docs/images/vorschau-energiefluss.png)

*Entwurf der Startseite (gerendert, Beispielwerte)*

## Funktionen

- **Energiefluss** wie in Solar.web: PV, Verbraucher, Netz und Batterie als Segmentringe
  um den Wechselrichter; Pfeile (oder auf Wunsch laufende Punkte) zeigen die Flussrichtung
- **Antippen öffnet Details:**
  - PV – Aufteilung je Wechselrichter (z. B. GEN24 + zusätzlicher Wechselrichter
    über Fronius Smart Meter), später Strings über Modbus
  - Netz – Phasen L1–L3 mit Spannung, Strom, Leistung, Frequenz
  - Batterie – Ladestand, Lade-/Entladeleistung, „reicht noch ca. …“ / „voll in ca. …“
  - Verbraucher – Haus, später Wallbox (Wattpilot) und Heizstab (Ohmpilot) getrennt
- **Tageswerte:** PV-Ertrag, Verbrauch, Netzbezug, Einspeisung, Batterie geladen,
  Autarkie, Eigenverbrauch – aus den Zählerständen selbst berechnet
  (der GEN24 liefert keine Tageswerte)
- **Ausrichtung wählbar:** USB-Anschluss links oder rechts (Drehung um 180°) –
  wirkt sofort, Bild und Touch drehen sich mit
- **Einrichtung komplett am Display:** WLAN, feste IP oder DHCP, IP des Wechselrichters,
  PV-Leistung (kWp, Vollausschlag des PV-Rings), Batteriekapazität (für die Restzeit)
- **Updates über WLAN** und Installation im Browser – wie beim
  [Violet Pool-Display](https://github.com/AlPi-BRV/violet-display)
- Dimmt nach 5 Minuten ohne Berührung; Detail- und Einstellungsseiten schließen sich nach 1 Minute

### Geplant

- Statistik für Tag, Monat, Jahr; optional mit **SD-Karte** auch Tagesverläufe
- Strings (MPPT) über Modbus TCP
- Wallbox (Wattpilot) mit eigener Abfrage

## Wie die Tageswerte entstehen

Der GEN24 liefert keine Tageswerte (`E_Day` ist leer). Das Display rechnet selbst:

| Wert | Quelle | Genauigkeit |
|---|---|---|
| Netzbezug, Einspeisung | Zählerstände des Smart Meters am Netzanschluss | genau |
| Weitere Wechselrichter (z. B. Hoymiles) | Zählerstand ihres Smart Meters | genau |
| Verbrauch, Autarkie | Zähler: Wechselrichter + weitere WR + Bezug − Einspeisung | genau |
| PV des Fronius, Batterie geladen/entladen | Leistung × Zeit, solange das Display läuft | Näherung |

Die Stände des Tagesbeginns werden alle 5 Minuten im Flash gesichert und überstehen
Stromausfall, Neustart und Updates. Die zählerbasierten Werte stimmen auch, wenn das
Display zwischendurch aus war; bei den Näherungswerten fehlt die Zeit ohne Strom.
Startet das Display mitten am Tag zum ersten Mal, zeigt „Heute“ die Werte *seit* dieser Uhrzeit.

## Voraussetzungen am Wechselrichter

Im Webinterface des Wechselrichters muss die **Solar API** eingeschaltet sein
(Kommunikation → Solar API). Test im Browser:

```
http://<IP-des-Wechselrichters>/solar_api/v1/GetPowerFlowRealtimeData.fcgi
```

Erscheint dort JSON, ist alles bereit.

## Bedienung

- **Kreis antippen** → Details (PV, Verbraucher, Netz mit Phasen L1–L3, Batterie mit Restzeit)
- **Tageswerte antippen** → Details Verbraucher
- **Zahnrad** oder Kopfzeile 2 Sekunden halten → Einstellungen
- Auf einer Detailseite irgendwohin tippen → zurück

| Reiter | Inhalt |
|---|---|
| Fronius | IP des Wechselrichters, PV-Leistung (kWp), Batterie (kWh), *Verbindung testen* |
| WLAN | Netz suchen und wählen, WLAN-Passwort |
| IP | Automatisch (DHCP) oder feste IP |
| Anzeige | Ausrichtung (normal / 180° gedreht), Flussrichtung als Pfeile oder laufende Punkte |
| System | Abfrageintervall (2 s bis 1 min), Update, Helligkeit & Kontrast, Werkseinstellungen |

## Hardware

| | |
|---|---|
| Display | **Elecrow CrowPanel ESP32 5.0"**, SKU **DIS07050H**, Board-Revision **V3.0**<br>oder **Elecrow CrowPanel ESP32 7.0"**, SKU **DIS08070H**, Board-Revision **V3.0** |
| Chip | ESP32-S3-WROOM-1-N4R8 (4 MB Flash, 8 MB PSRAM) |
| Auflösung | 800 × 480, kapazitiver Touch |

Beide Größen brauchen **jeweils eigene Firmware** – jede Datei trägt die Größe im Namen
(`fronius-display-5zoll-…` / `fronius-display-7zoll-…`).

## Selbst bauen (PlatformIO)

Wie beim Pool-Display: Ordner in VS Code mit PlatformIO öffnen, unten die Umgebung
**`env:crowpanel5`** oder **`env:crowpanel7`** wählen, *Upload*. Eigene Startwerte
(WLAN, IP des Wechselrichters) in `src/config_local.h` – wird von Git ignoriert:

```cpp
#define WIFI_SSID    "MeinWLAN"
#define WIFI_PASS    "geheim"
#define FRONIUS_HOST "192.168.1.60"
```

## Lizenz

MIT – siehe [LICENSE](LICENSE).
Verwendete Bibliotheken: [LovyanGFX](https://github.com/lovyan03/LovyanGFX),
[ArduinoJson](https://github.com/bblanchon/ArduinoJson).
Schrift: [Inter](https://rsms.me/inter/) (SIL Open Font License).
