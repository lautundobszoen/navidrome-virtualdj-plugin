# Navidrome für VirtualDJ

Ein nativer VirtualDJ-Online-Source für beliebige Navidrome-Server. Beim Verbinden trägt der Nutzer seine Serveradresse ein; das Plugin zeigt Playlists, Künstler und Genres im VirtualDJ-Browser an, unterstützt die VirtualDJ-Suche und streamt Tracks direkt aus Navidrome.

## Was angezeigt wird

VirtualDJ listet die Einträge mit den Präfixen `Playlist:`, `Künstler:` und `Genre:`. Beim Öffnen eines Eintrags erscheinen dessen Songs. Die Anmeldung fragt Benutzername und Passwort über den Windows-Anmeldedialog ab. Zugangsdaten können optional in der Windows-Anmeldeinformationsverwaltung gespeichert werden.

**SDK-Grenze:** Das öffentliche VirtualDJ-Online-Source-SDK bietet in seiner aktuellen Schnittstelle keine verschachtelten Unterordner für Drittanbieterquellen. Deshalb ist die Bibliothek in einer gemeinsamen, flachen Liste sortiert und nicht als `Playlists > ...`, `Artists > ...`, `Genres > ...` aufklappbarer Baum dargestellt. Die Kategorien und Einträge bleiben durch die Präfixe unterscheidbar.

**Listenabschluss:** `OnSearch` und `GetFolder` befüllen die Trackliste synchron und kehren danach mit `S_OK` zurück; bei synchronen Fehlern geben sie `E_FAIL` zurück. `IVdjTracksList::finish()` wird nicht aufgerufen, weil das SDK ihn für asynchrone `OnSearch`-Aufrufe vorsieht: Diese müssen `S_FALSE` zurückgeben und später `finish()` aufrufen. `S_FALSE` wird daher in den synchronen Listenpfaden nicht als Fehlercode verwendet.

## Serveradresse und Sicherheit

Beim Verbinden erscheint ein Dialog für die Serveradresse. Die zuletzt verwendete Adresse ist vorausgefüllt und kann jederzeit geändert werden. Gib die Basisadresse deiner Navidrome-Instanz ein, zum Beispiel `https://music.example.com` oder `https://example.com/navidrome` — **ohne** `/rest` am Ende und ohne Query-Parameter. HTTP und HTTPS werden unterstützt; für Verbindungen über das Internet wird HTTPS empfohlen. Bei HTTP weist das Plugin auf die unverschlüsselte Verbindung hin.

Die Adresse wird in der Windows-Benutzerregistrierung unter `HKEY_CURRENT_USER\Software\Cue\VirtualDJ\Navidrome` gespeichert. Gespeicherte Zugangsdaten werden je Serveradresse getrennt in der Windows-Anmeldeinformationsverwaltung abgelegt; dadurch werden Zugangsdaten verschiedener Instanzen nicht vermischt. Wird das Speichern im Anmeldedialog nicht gewählt, bleiben die Zugangsdaten nur solange das Plugin geladen ist.

Das Passwort wird nicht in der DLL oder im Projekt abgelegt. Für API-Aufrufe verwendet das Plugin einen Subsonic-Token statt des Passworts; Stream- und Cover-URLs enthalten diesen Token. Teile diese Links daher nicht. Bei HTTP können Zugangsdaten und Token im Netzwerk mitgelesen werden — verwende HTTP nur in einem vertrauenswürdigen lokalen Netzwerk.

## Voraussetzungen

- Windows 10/11, 64-Bit
- VirtualDJ 8 mit Pro-Lizenz: VirtualDJ dokumentiert Online-Source-Plugins als Pro-Funktion
- Visual Studio 2022 mit C++-Desktopentwicklung und CMake, falls der Build selbst ausgeführt wird

## Build

In PowerShell im entpackten Projektordner ausführen:

```powershell
.\build-windows.ps1
```

Die DLL wird unter `build\Release\Navidrome.dll` erzeugt. Alternativ:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Installation

1. VirtualDJ vollständig schließen.
2. Den Ordner `%USERPROFILE%\Documents\VirtualDJ\Plugins64\OnlineSources` anlegen, falls er noch nicht existiert.
3. `build\Release\Navidrome.dll` dorthin kopieren.
4. VirtualDJ neu starten und unter **Online Music** die Quelle **Navidrome** öffnen.
5. Beim Verbinden die Basisadresse des eigenen Navidrome-Servers eintragen und anschließend die Navidrome-Anmeldedaten eingeben. Die Option zum Speichern ist freiwillig.

## Unterstützte API-Funktionen

Das Plugin verwendet Navidromes Subsonic-kompatible API-Endpunkte für `getPlaylists`, `getArtists`, `getGenres`, `getPlaylist`, `getArtist`, `getAlbum`, `getSongsByGenre`, `search3`, `stream` und `getCoverArt`.

Das Paket enthält den Quellcode, aber keine fertige Windows-DLL. Die DLL muss auf Windows mit MSVC gebaut werden.

## Quellen und Komponenten

- [VirtualDJ Developer SDK](https://virtualdj.com/wiki/Developers)
- [VirtualDJ Online-Source-Schnittstelle](https://virtualdj.com/wiki/Plugins_SDKv8_OnlineSource.html)
- [VirtualDJ Online-Source-Beispiel](https://virtualdj.com/wiki/Plugins_SDKv8_Example7.html)
- [Navidrome Subsonic-API-Kompatibilität](https://www.navidrome.org/docs/developers/subsonic-api/)
- `include/vdjPlugin8.h`: offizieller VirtualDJ SDK-Basisheader
- `include/vdjOnlineSource.h`: Deklaration anhand der öffentlichen VirtualDJ-Schnittstellendokumentation
- `third_party/json.hpp`: nlohmann/json v3.11.3; MIT-Lizenz liegt daneben als `third_party/json.LICENSE`
