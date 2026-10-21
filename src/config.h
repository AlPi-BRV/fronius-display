#pragma once
// ---------------------------------------------------------------
// Startwerte – normalerweise muss hier NICHTS eingetragen werden.
//
// WLAN, IP des Wechselrichters, PV-Leistung und Batteriegroesse werden direkt
// am Display eingestellt (Zahnrad oder Kopfzeile 2 Sekunden gedrueckt halten)
// und dauerhaft gespeichert. Findet das Display beim Start kein WLAN, oeffnet
// es die WLAN-Einstellungen von selbst.
//
// Wer beim Entwickeln eigene Startwerte fest einbauen moechte, legt sie in
// src/config_local.h ab. Diese Datei wird von Git ignoriert, damit keine
// Passwoerter im Repository landen.
// ---------------------------------------------------------------

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID        ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS        ""
#endif

// IP-Adresse des Wechselrichters (ohne http://)
#ifndef FRONIUS_HOST
#define FRONIUS_HOST     ""
#endif

// Abfrageintervall in Millisekunden
#ifndef POLL_INTERVAL_MS
#define POLL_INTERVAL_MS 5000
#endif

// Zeitzone fuer Uhrzeit und Tageswechsel (POSIX-Format), Standard: Deutschland
#ifndef TIMEZONE
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
