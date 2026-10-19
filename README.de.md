# Steam Link Native Passthrough

<p align="center">
  <img src="docs/img/steam-link-icon.png" width="56" height="56" alt="Steam Link">
  &nbsp;&nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/img/steam-frame-logo-dark.svg">
    <img src="docs/img/steam-frame-logo.svg" width="240" alt="Steam Frame">
  </picture>
</p>

<p align="center">
  <a href="README.md"><img src="docs/img/flag-en.png" width="24" alt="English">&nbsp;English</a>
  &nbsp;&nbsp;|&nbsp;&nbsp;
  <img src="docs/img/flag-de.png" width="24" alt="Deutsch">&nbsp;<b>Deutsch</b>
</p>

> [!IMPORTANT]
> **Steam Frame SteamVR 2.18.2 · Build 1791331621**
>
> **Experimentelle PC- und Frame-Einrichtung:** Windows · NVIDIA · HEVC. Die genaue Laufzeitprüfung muss bestehen.

Spiele unterstützte PC-VR-Spiele über Steam Link in deinem echten Zimmer.

## Native Transparenz, kein Farbersatz

Das Spiel liefert seine Transparenz getrennt von den Farben; keine Farbe muss
aus dem Bild entfernt werden. Die Frame verbindet das Spiel mit ihrem eigenen
Kamerabild, auch bei Teiltransparenz; das Kamerabild bleibt auf der Brille.

**[Installieren oder aktualisieren](docs/INSTALL.de.md)** ·
**[Kompatibilität](docs/COMPATIBILITY.md)** ·
**[Deinstallieren oder zurücknehmen](docs/PC-FRAME-SETUP.md#update-uninstall-and-undo)**

## Unterstützte Spiele

| Spiel | Benötigte Mod | Mindestversion |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** (angekündigt) |

Andere Spiele benötigen eine eigene Integration. Der Patch allein ergänzt kein
Mixed Reality in einem unveränderten Spiel.

## Loslegen

Füge diese Zeile in **PowerShell auf deinem PC** ein:

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
```

1. Folge der [geführten Einrichtung](docs/PC-FRAME-SETUP.md#start-the-assistant).
   Sie zeigt installierte und verfügbare Patch-Versionen für PC und Frame und bietet Installation oder Update an.
2. Starte Steam Link und ein unterstütztes Spiel. Aktiviere dort natives
   Steam-Link-Passthrough. Mit derselben Option erhältst du das normale Bild zurück.

Der neue Weg lässt das vollständige Farbbild für das Spiel frei. **Die endgültige
Bildqualität und die gleichzeitige Darstellung von Spiel und Steam-Menü werden
noch auf der Brille geprüft.** Fehlt eine passende Komponente, bleibt das normale Videobild erhalten.

Nur dieser gekoppelte Weg wird weiterentwickelt und zur Installation angeboten.
Der frühere reine Frame-Empfänger ist [eingestellt](docs/history/ATLAS-RECEIVER.md).
Alte Releasearchive dokumentieren frühere Stände und sind keine empfohlene Alternative.

## Für Entwickler

[Spiel integrieren](docs/DEVELOPING.md) ·
[Protokoll und Laufzeitgrenzen](docs/COMPATIBILITY.md)

## Credits

Auf Grundlage von [Jake Luries steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Die Vorgeschichte und [MIT-Lizenz](LICENSE) bleiben erhalten; native Transparenz
wird hier von McFredward und Mitwirkenden entwickelt.

Inoffizielles Projekt, nicht mit Valve verbunden.
