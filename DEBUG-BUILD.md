# Retro Rewind Launcher – Debug-Build

Dieser Stand ist ein eigenständiger Debug-Build. Das Logging ist fest eingebaut und immer aktiv, es gibt keinen Schalter im Makefile. Gebaut wird wie gewohnt mit `make`.

## Bauen mit libogc 3.x

`source/ogc_compat.c` stellt `gettime()` und `diff_msec()` wieder als normale Funktionen bereit. Seit libogc 3.x sind das nur noch Inline-Funktionen. Der Launcher und das vorgebaute wii-mbedtls brauchen sie aber als Symbole, sonst bricht der Linker mit `undefined reference to 'gettime'` ab.

Wenn der Linker `multiple definition of 'inet_ntop'` bzw. `'inet_pton'` meldet, liegt das an libwiisocket und libogc 3.1.0, die beide diese Funktionen mitbringen. Abhilfe ohne die Makefile zu aendern: statt `make` einfach `./build.sh` ausfuehren (oder `./build.sh clean`). Das Skript ruft `make` mit angepasstem `LDFLAGS` auf. Alternativ in der `Makefile` (die Zeile mit `LDFLAGS`):

```
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map -Wl,--allow-multiple-definition
```

## Wo ist das Log?

- `sd:/RR_debug.log` im Root der SD-Karte. Die Datei wird bei jedem Start neu angelegt.
- Ohne nutzbare SD-Karte gibt es keine Log-Datei. Die letzten Warnungen und Fehler stehen dann auf dem Fehlerbildschirm.
- Schlägt das Schreiben mitten im Lauf fehl, wird die Datei geschlossen und der Grund steht auf dem Fehlerbildschirm.

Bis die SD-Karte gemountet ist (IOS-Reload, Video, SD-Init), landen die Zeilen im RAM und werden danach in die Datei geschrieben.

## Aufbau einer Zeile

```
[   3.214] E update.c:69 rrc_update_get_current_version | fopen("/RetroRewind6/version.txt", "r") FAILED: EIO (5: I/O error)
 Sekunden  Level  Datei:Zeile Funktion                   | Meldung
```

Level: `D` Debug, `I` Info, `W` Warnung, `E` Fehler, `F` Fatal.
Abschnitte des Startablaufs beginnen mit `==== `.

Am Ende jeder Datei steht eine **SUMMARY** (während des Laufs wird sie nach jedem Schreibvorgang aktualisiert, damit sie auch nach einem Absturz da ist; dann steht dort „state at the last write“) mit Laufzeit, Anzahl der Warnungen und Fehler sowie allen W/E/F-Zeilen in Reihenfolge. Dort am besten mit dem Lesen anfangen.

## Was protokolliert wird

- **Systeminfo:** IOS vor und nach dem Reload, Hollywood-Revision, Dolphin-Erkennung, Startpfad (argv), Video- und CONF-Werte, Speicher (Heap, MEM1/MEM2-Arenen), libcurl- und SSL-Version.
- **SD-Übersicht:** freier Platz, Clustergröße, Inhalt von `sd:/`, `/RetroRewind6`, `/RetroRewindChannel` und `/RetroRewind6/Binaries`, Größe aller wichtigen Dateien, `version.txt` als Hexdump.
- **Jeder Dateizugriff** (automatisch über `source/debugwrap.h`): `fopen`, `fclose` mit gelesenen und geschriebenen Bytes, Fehler bei `fread`/`fwrite`, `fseek`, `stat`, `opendir`, `mkdir`, `remove`, `rename`, `chdir`, `statvfs` und `read`, jeweils mit Aufrufstelle, Ergebnis und errno.
- **Automatische Diagnose** bei jedem echten Dateifehler. Bei `ENOENT` läuft sie nur, wenn der Code daraus wirklich einen Fehler macht. Die Diagnose umfasst:
  - stat der Datei und des übergeordneten Ordners
  - Ordnerinhalt mit Hinweis auf Groß-/Kleinschreibungs-Abweichungen
  - 3 Wiederholungsversuche. Klappt einer davon, war der Fehler nur vorübergehend.
  - freien Platz
  - einen Low-Level-Lesetest von Sektor 0 direkt über den SD-Treiber
  - offene Dateien (zur Leak-Erkennung)
  - den Heap-Zustand

  Das Ergebnis steht als Einzeiler `DIAGNOSIS verdict` im Log und als `Check:` auf dem Fehlerbildschirm.
- **Netzwerk:** `wiisocket_init`-Code, für jede HTTP-Anfrage die curl-Verbose-Ausgabe (Verbindung, TLS, Header), HTTP-Status, IP, Bytes, Dauer und curl-Fehlertext. Es gibt eine extra Warnung, wenn der Server 4xx/5xx liefert und der Launcher die Fehlerseite trotzdem als Daten behandelt.
- **Update:** Versionsdatei (Inhalt), jede Zeile mit benötigt/installiert, ZIP-Öffnen mit libzip-Fehlertext, jeder entpackte Eintrag, gelöschte Dateien.
- **DVD:** Cover-Register, Disc-ID, Partitionstabelle und jeder `DI_Read`. Bei Fehlern zusätzlich der Laufwerks-Fehlercode über `DI_GetError`.
- **Riivolution-XML:** XML-Parserfehler mit Zeilennummer (mxml), aktive Optionen und Patches, Anzahl der Ersetzungen pro Patch, fehlende Dateien, Ordnerfehler, die der Release-Build still ignoriert, sowie die Speicherbelegung in MEM1.
- **Loader:** runtime-ext-Pfad, Sections und Version, Loader.pul-Größe und -Ziel, DVD-Hooks.
- **UI:** jeder Prompt mit Text und Antwort, Tasten im Startbildschirm und in den Einstellungen. Ein Prompt, der wegen zu langer Zeilen nicht angezeigt werden kann, wird als Fehler geloggt.
- **Jeder Fehler** (`rrc_result`): wird beim Erzeugen mit Code-Stelle geloggt, beim Weiterreichen (`TRY`) und beim Anzeigen ebenfalls. Fehler, die verworfen werden, ohne je angezeigt zu werden, erscheinen als Warnung.
- **Abstürze (CPU-Exception):** Register, DAR/DSISR und ein Stack-Backtrace stehen auf dem Exception-Bildschirm (Foto machen). In die Log-Datei werden sie nicht geschrieben, weil der SD-Treiber in einer Exception nicht nutzbar ist. Die Log-Datei endet mit der letzten Zeile vor dem Absturz, und Warnungen und Fehler sind dort schon gespeichert.

## Was auf dem Bildschirm anders ist

- Unten links steht `DEBUG W:x E:y` mit einem Live-Zähler für Warnungen und Fehler. Die Farbe ist grün, gelb oder rot.
- Der fatale Fehlerbildschirm zeigt zusätzlich:
  - `Where:` mit Datei, Zeile und Funktion, in der der Fehler entstand
  - `Check:` mit dem Diagnose-Ergebnis
  - den Log-Status
  - die letzten Warnungen und Fehler

  Er bleibt 60 Sekunden stehen. A beendet sofort, sofern die Controller schon initialisiert sind.
- Normale Fehler-Prompts zeigen den genauen Code (z. B. `ERRNO EIO (5: I/O error) @ update.c:69`). Lange Texte werden umgebrochen, damit der Prompt immer erscheint. Der Release-Build zeigt bei zu langen Zeilen gar nichts an.
- Der Exception-Bildschirm bleibt 30 statt 10 Sekunden stehen.

## Backtrace-Adressen auflösen

```
powerpc-eabi-addr2line -f -C -e RR-Launcher-https.elf 80012345 80023456 ...
```

Wichtig ist die `.elf` aus genau diesem Build.

## Grenzen

- Das Log endet mit `closing log: launching the game`, bevor der Launcher das Spiel startet. Fehler im Spiel selbst (runtime-ext/Pulsar) protokolliert der Launcher nicht. Dafür gibt es `Crash.pul`.
- Bei einem Hard-Lock ohne Exception fehlen höchstens die letzten ca. 250 ms an Info-Zeilen. Warnungen und Fehler werden sofort auf die Karte geschrieben.

## Geänderte und neue Dateien

Neu:
- `source/debuglog.c`, `source/debuglog.h`: der Logger
- `source/debugwrap.c`, `source/debugwrap.h`: die automatischen Wrapper

Instrumentiert:
- `main.c`, `sd.c`, `result.c`/`.h`, `util.h`, `console.c`, `prompt.c`, `exception.c`/`.h`, `shutdown.c`, `crash.c`, `ephfile.c`, `settings.c`, `settingsfile.c`, `versionutils.c`, `di.c`, `gui.c`, `vec.c`
- `update/update.c`, `update/versionsfile.c`
- `loader/loader.c`, `loader/binary_loader.c`, `loader/disc_loader.c`, `loader/riivo_patch_loader.c`

Nicht angefasst: `runtime-ext/`, `loader/game_dol_loader.c`, `pngu/` und das Makefile.

Das Verhalten des Launchers ist bis auf die Anzeige gleich geblieben. Einzige echte Korrektur: `RRC_ASSERTEQ` wertet seinen Ausdruck nur noch einmal aus. Vorher wurde z. B. `IOS_ReloadIOS()` im Fehlerfall für die Meldung ein zweites Mal aufgerufen.

## Anzeige: Textbereich vergrößert

`_RRC_GUI_CON_WIDTH_4_3` (502 -> 572) und `_RRC_GUI_CON_HEIGHT_4_3` (300 -> 414) in `source/gui.c`. libogc 3.x behandelt die beiden Werte als rechte/untere Kante, ältere libogc-Versionen als Größe. Mit libogc 3.1.0 war der Textbereich deshalb nur 54x11 Zeichen groß (laut Log), gedacht sind 62x18. Jetzt sind es 62 Spalten und 18 Zeilen: `RR:` und `Channel:` stehen bündig unten rechts im Kasten, `DEBUG` unten links, Prompts sind mittig zentriert und Zeilen brechen bei 62 Zeichen um. Prompts und Fortschrittsanzeige haben feste Zeilen und sind sonst nicht betroffen.
