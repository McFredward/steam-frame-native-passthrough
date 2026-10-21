"""Small terminal presenter and bounded, read-only Steam Frame discovery.

Names, neighbor entries and SSH ports are hints, never device identity. The
caller must validate a real Frame inventory before returning a non-None value.
No credentials, persistence, service changes or installation exist here.
"""
from __future__ import annotations

from contextlib import contextmanager, nullcontext
from dataclasses import dataclass
import concurrent.futures
import ctypes
import ipaddress
import io
import json
import locale
import ntpath
import os
from pathlib import Path
import platform
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading
import textwrap
import time

PRIVATE = tuple(ipaddress.ip_network(v) for v in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))
NAMES = ('frame.local', 'steamframe.local', 'steam-frame.local')
MAX_VALIDATIONS = 8
MAX_SCAN_HOSTS = 254
MAX_NEIGHBORS = 64
VIRTUAL = re.compile(r'(?i)(vpn|wireguard|tailscale|zerotier|virtual|vmware|vbox|hyper-v|docker|podman|loopback|^lo$|^wsl|^tun\d*|^tap\d*|^veth|^virbr|^br-)')
TEXT = {
    'en': {
        'bridge_pc_only_stopping': 'Closing SteamVR on this PC; the Frame stays unchanged',
        'bridge_pc_only_plan': 'Only the PC patch needs changes. Setup closes SteamVR on this PC and updates its files while preserving the existing pairing. The Frame patch, settings and services stay unchanged; no Frame restart or picture check is needed.',
        'bridge_pc_only_installed': 'PC update complete. The Frame patch, settings and services were left unchanged; no Frame restart was performed.',
        'bridge_pc_health_ok': 'The updated PC files passed verification. The Frame remains unchanged.',
        'bridge_pc_rolled_back': 'The previous PC setup was restored. PC SteamVR stays closed. The Frame was left unchanged and was not restarted.',
        'bridge_pc_recovery_pending': 'An interrupted PC-only update has recovery records. Only the PC will be recovered; the Frame stays unchanged.',
        'bridge_pc_recover_confirm': 'Restore only the recorded PC setup, leaving the Frame unchanged?',
        'bridge_pc_pending_apply': 'Setup will recover the interrupted PC-only operation and apply the confirmed PC update. The Frame stays unchanged.',
        'bridge_pc_recovery_incomplete': 'PC recovery could not finish. Retry continues only the PC recovery; the Frame stays unchanged.',
        'bridge_pc_finalize_pending': 'The PC operation already finished. Retry clears only its remaining PC completion records; no patch is installed again and the Frame stays unchanged.',
        'bridge_pc_finalize_incomplete': 'The PC operation finished, but some PC completion records remain. Retry only clears these records; the Frame stays unchanged.',
        'bridge_stopping': 'Closing SteamVR on this PC; your Frame interface stays running',
        'bridge_starting': 'Briefly restarting Steam and SteamVR on the Frame, then checking the interface',
        'bridge_pc_stopped': 'SteamVR on this PC stays closed, including after recovery. Start your game normally when you are ready.',
        'bridge_rolled_back': 'The previous setup has been restored and its startup checks passed. PC SteamVR stays closed. The log and recovery paths are kept below.',
        'bridge_retry_question': 'Would you like to try again?',
        'bridge_connection_failed': 'The connection or sign-in failed. Details and the setup log show the reason; you can try again.',
        'bridge_cancelled_retained': 'Setup cancelled. No automatic rollback was started. Any pending recovery records were kept for your next setup run.',
        'bridge_reconnect': 'Reconnecting to your Frame after the restart',
        'bridge_retry_ambiguous': 'The last operation could not be confirmed. Setup will check what changed before offering a safe retry.',
        'bridge_resuming_update': 'The previous setup is restored. Continuing with the changes you already confirmed.',
        'bridge_inventory_refresh_failed': 'The operation finished, but the final status could not be refreshed. Run Check again to confirm the current installation.',
        'bridge_finalize_incomplete': 'You confirmed a usable Frame picture, but setup could not clear all completion records. No further restart or rollback was started. Retry only clears the remaining records. Exit keeps your setup log and any remaining recovery records.',
        'action_retry': 'Try again',
        'action_retry_description': 'Check the last result and try this step again.',
        'action_verify': 'Retry verification',
        'action_verify_description': 'Check the Frame startup again, then check its picture. Nothing is restarted or changed.',
        'action_manual': 'Enter Frame IP',
        'action_manual_description': 'Use the address shown in your Frame Wi-Fi settings.',
        'discovery_pc_only': 'Check this PC only',
        'discovery_pc_only_description': 'Continue without connecting to a Frame or changing either device.',
        'action_back': 'Back',
        'action_back_description': 'Return to the previous step.',
        'action_rollback': 'Restore previous setup',
        'action_rollback_description': 'Undo the recorded changes and restore the previous setup.',
        'action_continue': 'Continue',
        'action_continue_description': 'Continue with the plan shown above.',
        'choice_arrow_help': 'Up/Down or Home/End: move. Enter: select. Esc: cancel.',
        'choice_confirm': 'Confirm your choice',
        'confirm_no': 'No - go back',
        'confirm_yes': 'Yes - continue',
        'confirm_cancel': 'Cancel - keep installed components',
        'confirm_remove': 'Remove installed components',
        'confirm_yesno_prompt': 'Continue? [y/N]: ',
        'bridge_current': 'The checked connection is already up to date. Nothing was changed or restarted.',
        'bridge_available': 'This package includes the PC and Frame patches for native passthrough.',
        'bridge_remove_plan': 'Setup removes the current patch and any recorded older patch, returning Steam Link to its original unpatched state. Your Frame interface stays running during preparation, then Steam and SteamVR briefly restart and are checked. SteamVR on this PC may close; your game settings are kept.',
        'bridge_removed': 'The current patch and recorded older patch were removed. Steam Link is back to its original unpatched state.',
        'bridge_install_plan': 'Setup prepares the patch files while your Frame interface stays running. SteamVR on this PC may close. When ready, setup briefly restarts Steam and SteamVR on the Frame and checks the interface. Your game settings are kept. If startup fails, setup tries to restore the previous setup and restart its interface.',
        'bridge_confirm': 'Apply the changes shown above now?',
        'bridge_untested_confirm': 'Your SteamVR software has not been tested with this patch. Install and enable the patch anyway to try it? Compatibility is not guaranteed. You can uninstall the patch or restore the previous setup afterwards.',
        'bridge_untested_accept': 'Install anyway',
        'bridge_untested_versions': 'Untested SteamVR software detected',
        'bridge_untested_inactive': 'Installed, inactive on this unrecognized runtime',
        'bridge_untested_installed': 'The patch files were installed. Native passthrough remains inactive where the runtime could not be verified; ordinary Steam Link is unchanged. Update to a compatible patch to enable passthrough.',
        'bridge_partial': 'This action could not finish. Setup will try to restore the previous setup. The setup log shows the cause; log and recovery paths are listed below.',
        'setup_log': 'Setup log: ',
        'setup_log_unavailable': 'Setup log could not be created: ',
        'recovery_pc': 'PC recovery record (if created): ',
        'recovery_frame': 'Frame recovery record (if created): ',
        'bridge_installed': 'Setup complete. Steam and SteamVR on the Frame are running and their startup checks passed.',
        'bridge_recovery_pending': 'An earlier setup left recovery records. Setup must first restore the recorded previous setup before it can install or update. This attempt has not started making new changes.',
        'bridge_pending_apply': 'Setup is using the saved state from an unfinished earlier attempt to prepare these changes. The Frame interface stays running. It will restart once when preparation is complete; there is no separate recovery restart or picture question.',
        'bridge_recovery_incomplete': 'The previous setup could not be fully restored or verified. Check the setup log below. Retry continues restoration and may restart the Frame interface; Exit keeps the recovery records.',
        'bridge_recover_confirm': 'Restore the recorded original setup and restart its Frame interface now?',
        'bridge_started': 'Has the Frame interface returned and is it responsive?',
        'bridge_health_ok': 'The new startup passed the software checks. Please also check the picture in the headset.',
        'bridge_camera_warning': 'The Frame camera reported startup errors. Check the camera background as well as the menu before choosing Yes. Choose No if it is black, green or frozen; setup can check again or restore the previous setup. This warning does not trigger another restart.',
        'bridge_health_waiting': 'Startup is still being checked; no crash was confirmed. Retry checks again without restarting the Frame. You can also restore the previous setup.',
        'bridge_health_bad': 'The new startup failed its checks. Setup will try to restore the previous setup and check its restart.',
        'bridge_picture': 'Is the Frame interface usable and, when visible, is its camera view normal (not black or green)?',
        'bridge_original_picture': 'Is the previous Frame interface usable again and, when visible, is its camera view normal (not black or green)?',
        'bridge_picture_help': 'Choose No if the interface is unusable or the visible camera background is black, green, frozen or distorted. Setup checks again without restarting, then offers verification or restoring the previous setup. Cancel exits and keeps recovery records.',
        'bridge_original_picture_help': 'Choose No if the previous interface or its visible camera background has not returned normally. Setup can check again without restarting. Recovery records are kept until you confirm the picture. Cancel exits without another restart.',
        'picture_yes': 'Yes',
        'picture_no': 'No',
        'picture_cancel': 'Cancel setup',
        'picture_prompt': 'Picture normal? [y] Yes / [n] No / [c] Cancel setup (default No): ',
        'bridge_picture_rechecking': 'Checking the Frame startup again. No files or services are being changed.',
        'bridge_picture_checks_ok': 'The software checks passed again, but they cannot confirm the picture you see. Choose the next step below.',
        'bridge_picture_checks_unconfirmed': 'The startup could not be confirmed. This check did not restart or restore anything. Choose the next step below.',
        'bridge_stop_first': 'Setup prepares restoration while your Frame interface stays running, then briefly restarts Steam and SteamVR on the Frame and checks the interface. Recovery records are kept until restoration succeeds.',
        'bridge_continue': 'Continue with the supervised restoration? ',
        'bridge_unavailable': 'This package cannot install a supported PC and Frame connection here. Use Check only to review compatibility; existing components can be removed if shown below.',
        'remove_plan': 'This removes only the existing Frame patch. The PC stays unchanged, and Steam is not restarted. If SteamVR still has the patch loaded, stop it manually and run this setup again to finish.',
        'remove_done': 'The existing Frame patch has been removed. Steam was not restarted.',
        'remove_pending': 'The Frame patch is disabled, but removal is not finished. Stop SteamVR manually, then run this setup again. Recovery files are kept until restoration succeeds.',
        'confirm_uninstall': 'Remove the installed components shown above?',
        'confirm_remove_prompt': 'Type REMOVE to confirm (Enter to cancel): ',
        'confirm_remove_word': 'REMOVE',
        'actions': 'What would you like to do?',
        'action_install': 'Install',
        'action_install_description': 'Install the components included in this package.',
        'action_update': 'Update',
        'action_update_description': 'Update the existing installation with this package.',
        'action_repair': 'Repair',
        'action_repair_description': 'Reapply this patch version and check the Frame startup.',
        'action_uninstall': 'Uninstall',
        'action_uninstall_description': 'Remove the installed components.',
        'action_check': 'Check only',
        'action_check_description': 'Check this PC and Frame without changing them.',
        'action_exit': 'Exit',
        'action_exit_description': 'Close setup. Existing changes and recovery records are kept.',
        'recommended': 'recommended',
        'choice': 'Your choice',
        'choice_help': 'Enter = {default}.  0 = Exit.',
        'choice_invalid': 'Please enter one of the numbers shown, or 0 to exit.',
        'choice_cancelled': 'No action selected.',
        'pc_check': 'Checking SteamVR on this PC',
        'pc_ready': 'PC files match the supported version.',
        'pc_partial': 'One or more PC files could not be confirmed; details are in the report.',
        'receiver_ready': 'Frame software matches the supported version.',
        'receiver_changed': 'Frame software differs from the supported version; details are in the report.',
        'check_finish': 'Check complete. Nothing was installed, removed or restarted.',
        'report': 'Report saved: ',
        'done_prompt': 'Press Enter to close. ',
        'title': 'Steam Link Native Passthrough',
        'runtime_unknown': 'Unknown',
        'runtime_bundled': 'Bundled with SteamVR',
        'runtime_steamvr': 'SteamVR: {version}',
        'runtime_steam_link': 'Steam Link: {version}',
        'runtime_build': 'build {build}',
        'version_installed': 'Installed patch: {version}',
        'installer_version': 'Installer: {version}',
        'version_target': 'Available patch: {version}',
        'version_missing': 'Not installed',
        'version_unknown': 'Version unknown',
        'version_unchecked': 'Not checked',
        'version_plan': 'Patch versions to apply',
        'version_pc': 'This PC',
        'version_frame': 'Steam Frame',
        'version_downgrade': 'This package is older than an installed patch or has no version information. Update is unavailable. Use a newer package, or choose Restore previous setup if a saved recovery is available.',
        'inspection': 'Check only: no patch will be installed or removed, and nothing will be restarted.',
        'setup': 'We will check your PC and Frame before offering any changes.',
        'search': 'Looking for your Steam Frame',
        'nearby': 'Checking this local network',
        'verify': 'Checking the device',
        'found': 'Steam Frame connected.',
        'login': 'A possible Frame was found. Confirm its identity and sign in if asked.',
        'missing': 'We could not identify your Frame automatically. Keep it awake, on the same network, with SSH enabled.',
        'manual': 'Frame IP from its Wi-Fi settings (Enter to go back): ',
        'invalid': 'That address is not supported. Use the IP address shown in your Frame Wi-Fi settings, for example 192.168.1.20, or retry the search.',
        'unverified': 'Setup could not confirm this device as a Steam Frame. Check that it is awake and SSH is enabled, then retry or check the address.',
        'pc_only': 'Continuing with this PC only.',
        'done': 'done',
        'failed': 'could not finish',
        'cancelled': 'Setup stopped. Any recovery details are shown below.',
    },
    'de': {
        'bridge_pc_only_stopping': 'SteamVR auf diesem PC wird beendet; die Frame bleibt unverändert',
        'bridge_pc_only_plan': 'Nur der PC-Patch benötigt Änderungen. Die Einrichtung beendet SteamVR auf diesem PC und aktualisiert seine Dateien unter Beibehaltung der vorhandenen Kopplung. Patch, Einstellungen und Dienste auf der Frame bleiben unverändert; dort sind kein Neustart und keine Bildprüfung nötig.',
        'bridge_pc_only_installed': 'PC-Aktualisierung abgeschlossen. Patch, Einstellungen und Dienste auf der Frame blieben unverändert; die Frame wurde nicht neu gestartet.',
        'bridge_pc_health_ok': 'Die aktualisierten PC-Dateien haben die Prüfung bestanden. Die Frame bleibt unverändert.',
        'bridge_pc_rolled_back': 'Der vorherige PC-Zustand ist wiederhergestellt. PC-SteamVR bleibt geschlossen. Die Frame blieb unverändert und wurde nicht neu gestartet.',
        'bridge_pc_recovery_pending': 'Ein unterbrochenes reines PC-Update hat Wiederherstellungsdaten hinterlassen. Nur der PC wird wiederhergestellt; die Frame bleibt unverändert.',
        'bridge_pc_recover_confirm': 'Nur den gespeicherten PC-Zustand wiederherstellen und die Frame unverändert lassen?',
        'bridge_pc_pending_apply': 'Die Einrichtung stellt den unterbrochenen PC-Versuch wieder her und setzt die bestätigte PC-Aktualisierung um. Die Frame bleibt unverändert.',
        'bridge_pc_recovery_incomplete': 'Die PC-Wiederherstellung konnte nicht abgeschlossen werden. Erneut versuchen setzt nur diese fort; die Frame bleibt unverändert.',
        'bridge_pc_finalize_pending': 'Die PC-Aktion ist bereits abgeschlossen. Erneut versuchen bereinigt nur die verbliebenen PC-Abschlussdaten; kein Patch wird erneut installiert und die Frame bleibt unverändert.',
        'bridge_pc_finalize_incomplete': 'Die PC-Aktion ist abgeschlossen, aber einige PC-Abschlussdaten sind noch vorhanden. Erneut versuchen bereinigt nur diese; die Frame bleibt unverändert.',
        'bridge_stopping': 'SteamVR auf diesem PC wird beendet; die Frame-Oberfläche läuft weiter',
        'bridge_starting': 'Steam und SteamVR auf der Frame werden kurz neu gestartet; danach wird die Oberfläche geprüft',
        'bridge_pc_stopped': 'SteamVR auf diesem PC bleibt auch nach einer Wiederherstellung geschlossen. Starte dein Spiel wie gewohnt, sobald du bereit bist.',
        'bridge_rolled_back': 'Der vorherige Zustand ist wiederhergestellt und hat die Startprüfungen bestanden. PC-SteamVR bleibt geschlossen. Das Log und die Wiederherstellungspfade bleiben unten erhalten.',
        'bridge_retry_question': 'Möchtest du es erneut versuchen?',
        'bridge_connection_failed': 'Die Verbindung oder Anmeldung ist fehlgeschlagen. Details und Setup-Log zeigen den Grund; du kannst es erneut versuchen.',
        'bridge_cancelled_retained': 'Einrichtung abgebrochen. Es wurde keine automatische Rücknahme gestartet. Ausstehende Wiederherstellungsdaten bleiben für den nächsten Lauf erhalten.',
        'bridge_reconnect': 'Nach dem Neustart wieder mit der Frame verbinden',
        'bridge_retry_ambiguous': 'Die letzte Aktion konnte nicht bestätigt werden. Die Einrichtung prüft zunächst die Änderungen und bietet dann einen sicheren neuen Versuch an.',
        'bridge_resuming_update': 'Der Ausgangszustand ist wiederhergestellt. Die bereits bestätigten Änderungen werden jetzt fortgesetzt.',
        'bridge_inventory_refresh_failed': 'Die Aktion ist abgeschlossen, aber die abschließende Statusanzeige konnte nicht aktualisiert werden. Prüfe den Installationsstand mit „Nur prüfen“ erneut.',
        'bridge_finalize_incomplete': 'Du hast ein brauchbares Frame-Bild bestätigt, aber die Einrichtung konnte ihre Abschlussdaten nicht vollständig bereinigen. Es wurde keine weitere Rücknahme und kein weiterer Neustart gestartet. Erneut versuchen bereinigt nur die übrigen Abschlussdaten. Beenden behält dein Setup-Log und verbleibende Wiederherstellungsdateien.',
        'action_retry': 'Erneut versuchen',
        'action_retry_description': 'Das letzte Ergebnis prüfen und diesen Schritt erneut versuchen.',
        'action_verify': 'Prüfung wiederholen',
        'action_verify_description': 'Den Frame-Start erneut prüfen und danach das Bild kontrollieren. Dabei wird nichts neu gestartet oder verändert.',
        'action_manual': 'Frame-IP eingeben',
        'action_manual_description': 'Die Adresse aus den WLAN-Einstellungen deiner Frame verwenden.',
        'discovery_pc_only': 'Nur diesen PC prüfen',
        'discovery_pc_only_description': 'Ohne Frame-Verbindung fortfahren und keine Geräte ändern.',
        'action_back': 'Zurück',
        'action_back_description': 'Zum vorherigen Schritt zurückkehren.',
        'action_rollback': 'Vorherigen Zustand wiederherstellen',
        'action_rollback_description': 'Die protokollierten Änderungen zurücknehmen und den vorherigen Zustand wiederherstellen.',
        'action_continue': 'Weiter',
        'action_continue_description': 'Mit dem oben angezeigten Vorhaben fortfahren.',
        'choice_arrow_help': 'Hoch/Runter oder Pos1/Ende: bewegen. Enter: auswählen. Esc: abbrechen.',
        'choice_confirm': 'Auswahl bestätigen',
        'confirm_no': 'Nein - zurück',
        'confirm_yes': 'Ja - fortfahren',
        'confirm_cancel': 'Abbrechen - installierte Komponenten behalten',
        'confirm_remove': 'Installierte Komponenten entfernen',
        'confirm_yesno_prompt': 'Fortfahren? [j/N]: ',
        'bridge_current': 'Die geprüfte Verbindung ist bereits aktuell. Es wurde nichts geändert oder neu gestartet.',
        'bridge_available': 'Dieses Paket enthält die PC- und Frame-Patches für natives Passthrough.',
        'bridge_remove_plan': 'Setup entfernt den aktuellen und einen gegebenenfalls erfassten älteren Patch und stellt Steam Link ohne Patch wieder her. Während der Vorbereitung bleibt die Frame-Oberfläche aktiv. Danach werden Steam und SteamVR kurz neu gestartet und geprüft. SteamVR auf diesem PC wird gegebenenfalls beendet; deine Spieleinstellungen bleiben erhalten.',
        'bridge_removed': 'Der aktuelle und ein gegebenenfalls erfasster älterer Patch wurden entfernt. Steam Link läuft wieder im ursprünglichen Zustand ohne Patch.',
        'bridge_install_plan': 'Die Patch-Dateien werden bei laufender Frame-Oberfläche vorbereitet. SteamVR auf diesem PC wird gegebenenfalls beendet. Sobald alles bereit ist, werden Steam und SteamVR auf der Frame kurz neu gestartet und die Oberfläche geprüft. Deine Spieleinstellungen bleiben erhalten. Bei einem fehlgeschlagenen Start versucht die Einrichtung, den vorherigen Zustand wiederherzustellen und dessen Oberfläche neu zu starten.',
        'bridge_confirm': 'Die oben angezeigten Änderungen jetzt anwenden?',
        'bridge_untested_confirm': 'Deine SteamVR-Software wurde mit diesem Patch noch nicht getestet. Den Patch trotzdem installieren und aktivieren, um ihn auszuprobieren? Die Kompatibilität ist nicht garantiert. Du kannst den Patch danach deinstallieren oder den vorherigen Zustand wiederherstellen.',
        'bridge_untested_accept': 'Trotzdem installieren',
        'bridge_untested_versions': 'Ungetestete SteamVR-Software erkannt',
        'bridge_untested_inactive': 'Installiert, auf dieser unbekannten Laufzeit inaktiv',
        'bridge_untested_installed': 'Die Patch-Dateien wurden installiert. Wo die Laufzeit nicht verifiziert werden konnte, bleibt natives Passthrough inaktiv; normales Steam Link bleibt unverändert. Ein kompatibles Patch-Update kann Passthrough aktivieren.',
        'bridge_partial': 'Diese Aktion konnte nicht abgeschlossen werden. Die Einrichtung versucht, den vorherigen Zustand wiederherzustellen. Den Grund findest du im Setup-Log; Log- und Wiederherstellungspfade stehen unten.',
        'setup_log': 'Setup-Log: ',
        'setup_log_unavailable': 'Setup-Log konnte nicht erstellt werden: ',
        'recovery_pc': 'PC-Wiederherstellungsdatei (falls erstellt): ',
        'recovery_frame': 'Frame-Wiederherstellungsdatei (falls erstellt): ',
        'bridge_installed': 'Einrichtung abgeschlossen. Steam und SteamVR auf der Frame laufen und die Startprüfungen sind bestanden.',
        'bridge_recovery_pending': 'Eine frühere Einrichtung hat Wiederherstellungsdaten hinterlassen. Vor einer Installation oder Aktualisierung muss zuerst der damit gespeicherte vorherige Zustand wiederhergestellt werden. Dieser Versuch hat noch keine neuen Änderungen begonnen.',
        'bridge_pending_apply': 'Die Einrichtung nutzt den gespeicherten Zustand eines früheren, nicht abgeschlossenen Versuchs, um diese Änderungen vorzubereiten. Die Frame-Oberfläche läuft weiter. Nach der Vorbereitung wird sie einmal neu gestartet; es gibt keinen zusätzlichen Wiederherstellungs-Neustart und keine separate Bildabfrage.',
        'bridge_recovery_incomplete': 'Der vorherige Zustand konnte nicht vollständig wiederhergestellt oder geprüft werden. Sieh im Setup-Log unten nach. Erneut versuchen setzt die Wiederherstellung fort und kann die Frame-Oberfläche neu starten; Beenden behält die Wiederherstellungsdaten.',
        'bridge_recover_confirm': 'Den gespeicherten Ausgangszustand jetzt wiederherstellen und die Frame-Oberfläche neu starten?',
        'bridge_started': 'Ist die Frame-Oberfläche zurückgekehrt und reagiert sie?',
        'bridge_health_ok': 'Der neue Start hat die Softwareprüfungen bestanden. Bitte zusätzlich das Bild in der Brille prüfen.',
        'bridge_camera_warning': 'Die Frame-Kamera hat beim Start Fehler gemeldet. Prüfe vor Ja auch das Kamerabild hinter dem Menü. Wähle Nein, wenn es schwarz, grün oder eingefroren ist; die Einrichtung bietet eine erneute Prüfung oder die Wiederherstellung an. Diese Warnung löst keinen weiteren Neustart aus.',
        'bridge_health_waiting': 'Der Start wird noch geprüft; ein Absturz wurde nicht bestätigt. Erneut versuchen prüft weiter, ohne die Frame neu zu starten. Alternativ kannst du den vorherigen Zustand wiederherstellen.',
        'bridge_health_bad': 'Der neue Start hat die Prüfung nicht bestanden. Die Einrichtung versucht, den vorherigen Zustand wiederherzustellen und den Neustart zu prüfen.',
        'bridge_picture': 'Ist die Frame-Oberfläche bedienbar und das Kamerabild, falls sichtbar, normal (nicht schwarz oder grün)?',
        'bridge_original_picture': 'Ist die vorherige Frame-Oberfläche wieder bedienbar und das Kamerabild, falls sichtbar, normal (nicht schwarz oder grün)?',
        'bridge_picture_help': 'Wähle Nein, wenn die Oberfläche unbrauchbar oder das sichtbare Kamerabild schwarz, grün, eingefroren oder verzerrt ist. Die Einrichtung prüft ohne Neustart erneut und bietet danach eine weitere Prüfung oder Wiederherstellung an. Abbrechen beendet die Einrichtung und behält die Wiederherstellungsdateien.',
        'bridge_original_picture_help': 'Wähle Nein, wenn die vorherige Oberfläche oder das sichtbare Kamerabild noch nicht normal zurück ist. Die Einrichtung kann ohne Neustart erneut prüfen. Wiederherstellungsdateien bleiben bis zu deiner Bildbestätigung erhalten. Abbrechen beendet die Einrichtung ohne weiteren Neustart.',
        'picture_yes': 'Ja',
        'picture_no': 'Nein',
        'picture_cancel': 'Abbrechen',
        'picture_prompt': 'Bild normal? [j] Ja / [n] Nein / [a] Abbrechen (Standard Nein): ',
        'bridge_picture_rechecking': 'Der Frame-Start wird erneut geprüft. Dateien und Dienste werden nicht verändert.',
        'bridge_picture_checks_ok': 'Die Softwareprüfungen waren erneut erfolgreich, können das sichtbare Bild aber nicht bestätigen. Wähle unten den nächsten Schritt.',
        'bridge_picture_checks_unconfirmed': 'Der Start konnte nicht bestätigt werden. Diese Prüfung hat nichts neu gestartet oder wiederhergestellt. Wähle unten den nächsten Schritt.',
        'bridge_stop_first': 'Die Wiederherstellung wird bei laufender Frame-Oberfläche vorbereitet. Danach werden Steam und SteamVR auf der Frame kurz neu gestartet und die Oberfläche geprüft. Die Wiederherstellungsdaten bleiben bis zum erfolgreichen Abschluss erhalten.',
        'bridge_continue': 'Mit der überwachten Wiederherstellung fortfahren? ',
        'bridge_unavailable': 'Dieses Paket kann hier keine unterstützte PC- und Frame-Verbindung installieren. Wähle Nur prüfen, um die Kompatibilität zu prüfen. Vorhandene Komponenten lassen sich entfernen, wenn unten angeboten.',
        'remove_plan': 'Es wird nur der vorhandene Frame-Patch entfernt. Der PC bleibt unverändert und Steam wird nicht neu gestartet. Falls SteamVR den Patch noch geladen hat, bitte manuell beenden und diese Einrichtung erneut starten, um das Entfernen abzuschließen.',
        'remove_done': 'Der vorhandene Frame-Patch wurde entfernt. Steam wurde nicht neu gestartet.',
        'remove_pending': 'Der Frame-Patch ist deaktiviert, aber noch nicht vollständig entfernt. Bitte SteamVR manuell beenden und diese Einrichtung erneut starten. Die Wiederherstellungsdateien bleiben bis zum erfolgreichen Abschluss erhalten.',
        'confirm_uninstall': 'Die oben angezeigten installierten Komponenten entfernen?',
        'confirm_remove_prompt': 'Zum Bestätigen ENTFERNEN eingeben (Enter zum Abbrechen): ',
        'confirm_remove_word': 'ENTFERNEN',
        'actions': 'Was möchtest du tun?',
        'action_install': 'Installieren',
        'action_install_description': 'Die in diesem Paket enthaltenen Komponenten installieren.',
        'action_update': 'Aktualisieren',
        'action_update_description': 'Die vorhandene Installation mit diesem Paket aktualisieren.',
        'action_repair': 'Reparieren',
        'action_repair_description': 'Diese Patch-Version erneut anwenden und den Frame-Start prüfen.',
        'action_uninstall': 'Deinstallieren',
        'action_uninstall_description': 'Die installierten Komponenten entfernen.',
        'action_check': 'Nur prüfen',
        'action_check_description': 'PC und Frame prüfen, ohne etwas zu ändern.',
        'action_exit': 'Beenden',
        'action_exit_description': 'Die Einrichtung schließen. Vorhandene Änderungen und Wiederherstellungsdateien bleiben erhalten.',
        'recommended': 'empfohlen',
        'choice': 'Deine Auswahl',
        'choice_help': 'Enter = {default}.  0 = Beenden.',
        'choice_invalid': 'Bitte eine angezeigte Nummer oder 0 zum Beenden eingeben.',
        'choice_cancelled': 'Keine Aktion ausgewählt.',
        'pc_check': 'SteamVR auf diesem PC wird geprüft',
        'pc_ready': 'Die PC-Dateien entsprechen der unterstützten Version.',
        'pc_partial': 'Einige PC-Dateien konnten nicht bestätigt werden; Einzelheiten stehen im Bericht.',
        'receiver_ready': 'Die Frame-Software entspricht der unterstützten Version.',
        'receiver_changed': 'Die Frame-Software weicht von der unterstützten Version ab; Einzelheiten stehen im Bericht.',
        'check_finish': 'Prüfung abgeschlossen. Es wurde nichts installiert, entfernt oder neu gestartet.',
        'report': 'Bericht gespeichert: ',
        'done_prompt': 'Zum Schließen Enter drücken. ',
        'title': 'Steam Link Native Passthrough',
        'runtime_unknown': 'Unbekannt',
        'runtime_bundled': 'Mit SteamVR geliefert',
        'runtime_steamvr': 'SteamVR: {version}',
        'runtime_steam_link': 'Steam Link: {version}',
        'runtime_build': 'Build {build}',
        'version_installed': 'Installierter Patch: {version}',
        'installer_version': 'Installer: {version}',
        'version_target': 'Verfügbarer Patch: {version}',
        'version_missing': 'Nicht installiert',
        'version_unknown': 'Version unbekannt',
        'version_unchecked': 'Nicht geprüft',
        'version_plan': 'Anzuwendende Patch-Versionen',
        'version_pc': 'Dieser PC',
        'version_frame': 'Steam Frame',
        'version_downgrade': 'Dieses Paket ist älter als ein installierter Patch oder hat keine Versionsangabe. Eine Aktualisierung ist damit nicht möglich. Nutze ein neueres Paket oder wähle Vorherigen Zustand wiederherstellen, falls entsprechende Wiederherstellungsdaten verfügbar sind.',
        'inspection': 'Nur prüfen: Es wird kein Patch installiert oder entfernt und nichts neu gestartet.',
        'setup': 'Wir prüfen PC und Frame, bevor Änderungen angeboten werden.',
        'search': 'Steam Frame wird gesucht',
        'nearby': 'Lokales Netzwerk wird geprüft',
        'verify': 'Gerät wird geprüft',
        'found': 'Steam Frame verbunden.',
        'login': 'Eine mögliche Frame wurde gefunden. Bestätige bei Nachfrage die Gerätekennung und melde dich an.',
        'missing': 'Die Frame wurde nicht automatisch erkannt. Bitte eingeschaltet lassen, im selben Netzwerk verbinden und SSH aktivieren.',
        'manual': 'Frame-IP aus den WLAN-Einstellungen (Enter für zurück): ',
        'invalid': 'Diese Adresse wird nicht unterstützt. Nutze die IP-Adresse aus den WLAN-Einstellungen deiner Frame, zum Beispiel 192.168.1.20, oder starte die Suche erneut.',
        'unverified': 'Die Einrichtung konnte dieses Gerät nicht als Steam Frame bestätigen. Prüfe, ob es eingeschaltet und SSH aktiviert ist, und versuche es erneut oder prüfe die Adresse.',
        'pc_only': 'Es wird nur dieser PC geprüft.',
        'done': 'fertig',
        'failed': 'nicht abgeschlossen',
        'cancelled': 'Einrichtung beendet. Etwaige Wiederherstellungsdetails stehen unten.',
    },
}


def language(override=None):
    if override is not None:
        return 'de' if str(override).lower().split('-')[0].split('_')[0] == 'de' else 'en'
    try:
        value = locale.getlocale()[0] or ''
    except (ValueError, TypeError):
        value = ''
    if not value:
        value = os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or os.environ.get('LANG', '')
    return 'de' if value.lower().startswith(('de', 'german')) else 'en'


def version_labels(text,record):
    """Render checked metadata; absence never invents an installed version."""
    def safe(value):
        return value if type(value)is str and re.fullmatch(r'[0-9A-Za-z][0-9A-Za-z.+-]{0,79}',value) else None
    current=safe(record.get('installed_version'))
    if record.get('installed') is False:current=text('version_missing')
    elif record.get('installed') is not True:current=text('version_unchecked')
    elif current is None:current=text('version_unknown')
    return current,safe(record.get('target_version')) or text('version_unknown')


def installer_label(text,report):
    version=report.get('installer_version') if type(report)is dict else None
    if type(version)is str and re.fullmatch(r'[0-9A-Za-z][0-9A-Za-z.+-]{0,79}',version):
        return text('installer_version').format(version=version)
    return None


def runtime_labels(text, record):
    """Presentation metadata is not compatibility or installation authority."""
    versions = record.get('runtime_versions')
    if type(versions) is not dict:
        return []
    rows = []
    steamvr_known = False
    for component in ('steamvr', 'steam_link'):
        item = versions.get(component)
        item = item if type(item) is dict else {}
        version, build = item.get('version'), item.get('build')
        if type(version) is not str or not re.fullmatch(r'[0-9]{1,5}(?:[.][0-9]{1,10}){1,3}', version):
            version = None
        if type(build) is not str or not re.fullmatch(r'[0-9]{1,20}', build):
            build = None
        if component == 'steamvr':
            steamvr_known = bool(version or build)
        label = version or text('runtime_unknown')
        if component == 'steam_link' and not version and not build and steamvr_known:
            # This is a package relationship; the SteamVR label above carries
            # its actual version/build. Never invent a separate Link version.
            label = text('runtime_bundled')
        if build:
            label = version + ' (' + build + ')' if version else text('runtime_build').format(build=build)
        rows.append(text('runtime_' + component).format(version=label))
    return rows


def version_plan(text,pc,frame):
    rows=[text('version_plan')]
    for role,record in (('pc',pc),('frame',frame)):
        current,target=version_labels(text,record)
        rows.append(text('version_'+role)+': '+current+' → '+target)
    return '\n'.join(rows)


def untested_runtime_plan(text,pc,frame):
    """Show detected versions without promoting their labels to ABI permission."""
    rows=[text('bridge_untested_versions')]
    for role,versions in (('pc',pc),('frame',frame)):
        rows.append(text('version_'+role))
        rows.extend(runtime_labels(text,{'runtime_versions':versions}) or [text('runtime_unknown')])
    return '\n'.join(rows)


class _WindowsKeyReader:
    """CRT console keys; no line input, terminal subprocess or dependency."""
    def __init__(self, getwch=None):
        if getwch is None:
            import msvcrt
            getwch = msvcrt.getwch
        self._getwch = getwch

    def read(self):
        char = self._getwch()
        if char in ('\x00', '\xe0'):
            return {'H': 'up', 'P': 'down', 'G': 'home', 'O': 'end'}.get(self._getwch(), 'ignore')
        if char == '\x03':
            raise KeyboardInterrupt()
        if char in ('', '\x1a'):
            raise EOFError()
        return {'\r': 'enter', '\n': 'enter', '\x1b': 'escape'}.get(char, 'ignore')


class _PosixKeyReader:
    def __init__(self, descriptor):
        self.descriptor = descriptor

    def read(self):
        char = os.read(self.descriptor, 1)
        if not char or char == b'\x04':
            raise EOFError()
        if char == b'\x03':
            raise KeyboardInterrupt()
        if char in (b'\r', b'\n'):
            return 'enter'
        if char != b'\x1b':
            return 'ignore'
        # This is only Escape/CSI disambiguation, never an operation timer.
        sequence = bytearray()
        for _ in range(16):
            if not select.select([self.descriptor], [], [], .08)[0]:
                break
            part = os.read(self.descriptor, 1)
            if not part:
                break
            sequence.extend(part)
            if len(sequence) == 1 and part not in (b'[', b'O'):
                break
            if len(sequence) >= 2 and (part.isalpha() or part == b'~'):
                break
        return {b'[A': 'up', b'OA': 'up', b'[B': 'down', b'OB': 'down',
                b'[H': 'home', b'OH': 'home', b'[1~': 'home', b'[7~': 'home',
                b'[F': 'end', b'OF': 'end', b'[4~': 'end', b'[8~': 'end'}.get(bytes(sequence), 'escape' if not sequence else 'ignore')


class _ArrowTerminal:
    """Temporary console mode; always restore it before returning an action.

    ANSI cursor movement is restricted to an actual terminal. Windows explicitly
    enables/restores its own output console mode; pipes use the plain presenter.
    The menu never clears the screen, hides the cursor or enters another buffer.
    """
    def __init__(self, source, stream):
        self.source, self.stream = source, stream
        self._restore = None
        self.restored = False
        self.rows = 0

    def __enter__(self):
        if os.name == 'nt':
            import msvcrt
            from ctypes import wintypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            get_mode, set_mode = kernel.GetConsoleMode, kernel.SetConsoleMode
            get_mode.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
            get_mode.restype = wintypes.BOOL
            set_mode.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            set_mode.restype = wintypes.BOOL
            handle = msvcrt.get_osfhandle(self.stream.fileno())
            original = wintypes.DWORD()
            if not get_mode(handle, ctypes.byref(original)) or not set_mode(handle, original.value | 4):
                raise OSError('terminal mode unavailable')
            self._restore = lambda: bool(set_mode(handle, original.value))
            try:
                self.reader = _WindowsKeyReader()
            except BaseException:
                self.__exit__()
                raise
        else:
            import termios
            descriptor = self.source.fileno()
            original = termios.tcgetattr(descriptor)
            changed = list(original)
            changed[6] = list(original[6])
            changed[3] &= ~(termios.ICANON | termios.ECHO)
            changed[6][termios.VMIN], changed[6][termios.VTIME] = 1, 0
            termios.tcsetattr(descriptor, termios.TCSANOW, changed)
            self._restore = lambda: termios.tcsetattr(descriptor, termios.TCSANOW, original) is None
            self.reader = _PosixKeyReader(descriptor)
        return self

    def draw(self, presenter, lines):
        with presenter._lock:
            old = self.rows
            if old:
                presenter._write('\x1b[' + str(old) + 'A')
            for index in range(max(old, len(lines))):
                presenter._write('\r\x1b[2K' + (lines[index] if index < len(lines) else '') + '\n')
            if old > len(lines):
                presenter._write('\x1b[' + str(old - len(lines)) + 'A')
            self.rows = len(lines)

    def __exit__(self, *_):
        if self._restore is not None:
            try:
                self.restored = self._restore()
            except (OSError, ValueError):
                self.restored = False
        return False


class Presenter:
    """Arrow-key menus and animation exist only on a real terminal.

    Plain OpenSSH/password operations use suspend() so the progress writer
    cannot race a terminal prompt. The Textual presenter handles authentication
    inside its own app and must not release the terminal for those prompts.
    """
    inline_ssh = False

    def __new__(cls, *args, **kwargs):
        # Only the actual entry point owns Textual's terminal lifetime. Discovery
        # and workflow retries reuse its one presenter across sibling imports.
        runtime = sys.modules.get('_sfnp_setup_textual')
        if runtime is not None:
            active = runtime.active_presenter(lang=kwargs.get('lang'))
            if active is not None:
                return active
        return super().__new__(cls)

    def __init__(self, stream=None, input_fn=input, lang=None, input_enabled=None, width=None):
        self.stream = sys.stdout if stream is None else stream
        self.input_fn = input_fn
        self.lang = language(lang)
        self.interactive = bool(getattr(self.stream, 'isatty', lambda: False)())
        self.prompting = bool(sys.stdin.isatty()) if input_enabled is None else bool(input_enabled)
        self._terminal_width = width
        self._lock = threading.RLock()
        self._tasks = []
        self._paused = 0
        self._width = 0
        self._stop = threading.Event()
        self._thread = None

    def text(self, key):
        return TEXT[self.lang][key]

    def _write(self, value):
        encoding = getattr(self.stream, 'encoding', None)
        if encoding:
            value = value.encode(encoding, errors='replace').decode(encoding)
        self.stream.write(value)
        self.stream.flush()

    def columns(self):
        """Leave the terminal's final column unused to avoid implicit wrapping."""
        width = self._terminal_width
        if width is None:
            try:
                width = os.get_terminal_size(self.stream.fileno()).columns
            except (AttributeError, OSError, ValueError, io.UnsupportedOperation):
                width = shutil.get_terminal_size(fallback=(80, 24)).columns
        # PTYs/redirected consoles may report an unset zero-sized window.
        # Treat that as unknown, rather than collapsing every paragraph.
        width = int(width)
        return max(12, min(width if width > 0 else 80, 120)) - 1

    def _paragraph(self, value, *, indent=2):
        prefix = ' ' * indent
        value = str(value)
        lines = textwrap.wrap(value, width=max(1, self.columns() - indent),
                              break_long_words=True, break_on_hyphens=False,
                              replace_whitespace=True) or ['']
        self._write(''.join(prefix + line + '\n' for line in lines))

    def _clear(self):
        if self._width:
            self._write('\r' + ' ' * min(self._width, self.columns()) + '\r')
            self._width = 0

    def say(self, key):
        with self._lock:
            self._clear()
            self._paragraph(self.text(key))
            self._write('\n')

    def problem(self, message):
        """Display only the caller's tagged, authored public diagnostic."""
        with self._lock:
            self._clear()
            self._paragraph(str(message)[:500])
            self._write('\n')

    def section(self, key):
        with self._lock:
            self._clear()
            self._write('\n')
            self._paragraph(self.text(key))
            self._write('  ' + '-' * min(48, self.columns() - 2) + '\n\n')

    def status(self, key, state='info'):
        """A caller-classified status, without inferring installation readiness."""
        if state not in ('info', 'ok', 'warning'):
            raise ValueError('unknown status state')
        with self._lock:
            self._clear()
            marker = {'info': '[i]', 'ok': '[ok]', 'warning': '[!]'}[state]
            self._paragraph(marker + ' ' + self.text(key))
            self._write('\n')

    def path(self, value, key='report'):
        """Display the label and path separately; never append it to a status row."""
        with self._lock:
            self._clear()
            self._paragraph(self.text(key).strip())
            # Do not let a path containing control characters move the cursor.
            value = ''.join(c if c.isprintable() else '?' for c in str(value))
            self._paragraph(value, indent=4)
            self._write('\n')

    def banner(self, inspection=True):
        with self._lock:
            self._write('\n')
            if self.columns() >= 28:
                self._write('  +------------------------+\n'
                            '  |  [o]  STEAM FRAME  [o] |\n'
                            '  +------------------------+\n\n')
            else:
                self._paragraph('[o] FRAME [o]')
                self._write('\n')
            self._paragraph(self.text('title'))
            self._write('\n')
        self.say('inspection' if inspection else 'setup')

    def inventory(self,*,pc=None,frame=None,report=None):
        if type(report)is dict:pc=report.get('pc',pc);frame=report.get('frame',frame)
        with self._lock:
            label=installer_label(self.text,report)
            if label:self._paragraph(label)
            for role,value in (('pc',pc),('frame',frame)):
                if type(value)is not dict or not {'installed_version','target_version','runtime_versions'}&set(value):continue
                current,target=version_labels(self.text,value)
                self._paragraph(self.text('version_'+role))
                for row in runtime_labels(self.text,value):self._paragraph(row,indent=4)
                if {'installed_version','target_version'}&set(value):
                    self._paragraph(self.text('version_installed').format(version=current),indent=4)
                    self._paragraph(self.text('version_target').format(version=target),indent=4)
                if value.get('installed') is True and value.get('activation_mode')=='inactive':
                    self._paragraph(self.text('bridge_untested_inactive'),indent=4)

    def version_plan(self,*,pc,frame):
        with self._lock:
            for line in version_plan(self.text,pc,frame).splitlines():self._paragraph(line)

    def untested_runtime(self,*,pc,frame):
        with self._lock:
            for line in untested_runtime_plan(self.text,pc,frame).splitlines():self._paragraph(line)

    def _animate(self, stopped):
        phase = 0
        while not stopped.wait(.12):
            with self._lock:
                if self._paused or not self._tasks:
                    continue
                value = '  ' + '|/-\\'[phase % 4] + ' ' + self._tasks[-1]
                width = self.columns()
                if len(value) > width:
                    value = value[:max(0, width - 3)] + '...'
                self._write('\r' + value + ' ' * max(0, min(self._width, width) - len(value)))
                self._width = len(value)
                phase += 1

    @contextmanager
    def progress(self, key):
        label = self.text(key)
        with self._lock:
            self._tasks.append(label)
            if self.interactive:
                if self._thread is None:
                    self._stop = threading.Event()
                    self._thread = threading.Thread(target=self._animate, args=(self._stop,), name='sfnp-progress', daemon=True)
                    self._thread.start()
            else:
                self._write('\n')
                self._paragraph(label + '...')
                self._write('\n')
        okay = False
        try:
            yield
            okay = True
        finally:
            with self._lock:
                self._tasks.pop()
                self._clear()
                self._paragraph('[' + self.text('done' if okay else 'failed') + '] ' + label)
                self._write('\n')
                worker = None
                if not self._tasks and self._thread is not None:
                    self._stop.set()
                    worker, self._thread = self._thread, None
            if worker is not None:
                worker.join()

    @contextmanager
    def suspend(self):
        with self._lock:
            self._paused += 1
            self._clear()
        try:
            yield
        finally:
            with self._lock:
                self._paused -= 1

    def _input(self, label):
        with self.suspend():
            with self._lock:
                if len(label) + 2 <= self.columns():
                    prompt = '  ' + label
                else:
                    self._paragraph(label)
                    prompt = '  > '
            return self.input_fn(prompt)

    def ask(self, key):
        if not self.prompting:
            return ''
        return self._input(self.text(key))

    def _arrow_capable(self):
        # Input permission and output animation are separate; a redirected
        # output with an interactive input still gets a usable plain prompt.
        return (self.prompting and self.interactive and
                bool(getattr(sys.stdin, 'isatty', lambda: False)()) and
                (os.name == 'nt' or os.environ.get('TERM', '') != 'dumb'))

    def _menu_lines(self, choices, selected, recommended, title):
        width = self.columns()
        try:
            height = os.get_terminal_size(self.stream.fileno()).lines or 24
        except (AttributeError, OSError, ValueError, io.UnsupportedOperation):
            height = 24
        visible = min(len(choices), max(2, min(height, 60) - 16))
        first = min(max(0, selected - visible + 1), max(0, len(choices) - visible))
        def short(value):
            value = ''.join(c if c.isprintable() else '?' for c in value)
            return value if len(value) <= width else value[:max(0, width - 3)] + '...'
        lines = ['  +' + '-' * max(1, width - 4) + '+']
        lines.extend('  ' + line for line in textwrap.wrap(self.text(title), width=max(1, width - 2))[:3])
        lines.append('')
        if first:
            lines.append('    ...')
        for index in range(first, first + visible):
            value = ('  > ' if index == selected else '    ') + choices[index][1]
            if choices[index][0] == recommended:
                value += ' [' + self.text('recommended') + ']'
            lines.append(short(value))
        if first + visible < len(choices):
            lines.append('    ...')
        description = choices[selected][2]
        if description:
            lines.append('')
            wrapped = textwrap.wrap(description, width=max(1, width - 4))
            lines.extend('    ' + line for line in wrapped[:2])
            if len(wrapped) > 2:
                lines[-1] = short(lines[-1][:-3] + '...')
        lines.append('')
        wrapped = textwrap.wrap(self.text('choice_arrow_help'), width=max(1, width - 2))
        lines.extend('  ' + line for line in wrapped[:3])
        lines.extend(['  +' + '-' * max(1, width - 4) + '+', ''])
        return lines

    def _arrow_choose(self, choices, default, recommended=None, title='actions'):
        """None means cancel; False means console mode could not be prepared."""
        if default is None:
            choices = [(None, self.text('choice_cancelled'), ''), *choices]
        terminal = _ArrowTerminal(sys.stdin, self.stream)
        try:
            terminal.__enter__()
        except (AttributeError, OSError, ValueError, io.UnsupportedOperation):
            return False
        selected = next((i for i, value in enumerate(choices) if value[0] == default), 0)
        answer = None
        try:
            while True:
                terminal.draw(self, self._menu_lines(choices, selected, recommended, title))
                key = terminal.reader.read()
                if key == 'escape':
                    break
                if key == 'enter':
                    answer = choices[selected][0]
                    break
                if key == 'up':
                    selected = max(0, selected - 1)
                elif key == 'down':
                    selected = min(len(choices) - 1, selected + 1)
                elif key == 'home':
                    selected = 0
                elif key == 'end':
                    selected = len(choices) - 1
        except (EOFError, KeyboardInterrupt, OSError):
            pass
        finally:
            terminal.__exit__()
        return answer if terminal.restored else None

    def choose_action(self, actions, recommended=None):
        """Offer only caller-approved actions; cancellation never implies a write.

        Real consoles use arrow keys and Enter. Pipes retain a numbered menu.
        This selects an action; the engine still owns its plan and permissions.
        """
        known = ('install', 'update', 'repair', 'uninstall', 'check', 'exit',
                 'retry', 'verify', 'back', 'rollback', 'continue', 'manual')
        if (type(actions) not in (list, tuple) or len(actions) > len(known) or
                any(type(action) is not str or action not in known for action in actions) or
                len(set(actions)) != len(actions)):
            raise ValueError('actions must be distinct supported action names')
        if not actions or not self.prompting:
            return None
        default = recommended if recommended in actions else ('check' if 'check' in actions else ('exit' if 'exit' in actions else None))
        discovery = {'retry', 'manual', 'check', 'exit'} <= set(actions)
        def action_words(action):
            key = 'discovery_pc_only' if discovery and action == 'check' else 'action_' + action
            return self.text(key), self.text(key + '_description')
        with self.suspend():
            if self._arrow_capable():
                choices = [(action, *action_words(action)) for action in actions]
                selected = self._arrow_choose(choices, default, recommended)
                if selected is not False:
                    return selected
            self.section('actions')
            with self._lock:
                for number, action in enumerate(actions, 1):
                    label, description = action_words(action)
                    if action == recommended:
                        label += ' [' + self.text('recommended') + ']'
                    self._paragraph(str(number) + '. ' + label)
                    self._paragraph(description, indent=5)
                    self._write('\n')
                self._paragraph(self.text('choice_help').format(default=action_words(default or 'exit')[0]))
                self._write('\n')
            number = actions.index(default) + 1 if default is not None else 0
            label = self.text('choice') + ' [' + str(number) + ']: '
            for _ in range(3):
                try:
                    answer = self._input(label).strip()
                except (EOFError, KeyboardInterrupt):
                    self.say('choice_cancelled')
                    return None
                if not answer:
                    return default
                if answer.lower() in ('0', 'q', 'exit', 'cancel', 'abbrechen'):
                    return None
                if answer.isascii() and answer.isdigit() and 1 <= len(answer) <= 2 and 1 <= int(answer) <= len(actions):
                    return actions[int(answer) - 1]
                self.say('choice_invalid')
            self.say('choice_cancelled')
            return None

    def confirm(self, key):
        """Explicit permission for a displayed plan; No is always the default."""
        if key in ('bridge_picture','bridge_original_picture'):
            return self.picture(key)
        if not self.prompting:
            return False
        with self.suspend():
            self.say(key)
            if self._arrow_capable():
                choices = [('no', self.text('confirm_no'), ''),
                           ('yes', self.text('bridge_untested_accept' if key=='bridge_untested_confirm' else 'confirm_yes'), '')]
                result = self._arrow_choose(choices, 'no', title='choice_confirm')
                if result is not False:
                    return result == 'yes'
            try:
                answer = self._input(self.text('confirm_yesno_prompt')).strip().casefold()
            except (EOFError, KeyboardInterrupt):
                return False
            return answer in ('y', 'yes', 'j', 'ja')

    def picture(self,key):
        """Return Yes, No or cancellation distinctly on every presentation."""
        if not self.prompting:return None
        with self.suspend():
            self.say(key)
            self.say('bridge_original_picture_help' if key=='bridge_original_picture' else 'bridge_picture_help')
            if self._arrow_capable():
                choices=[('cancel',self.text('picture_cancel'),''),
                         ('no',self.text('picture_no'),''),('yes',self.text('picture_yes'),'')]
                result=self._arrow_choose(choices,'no',title='choice_confirm')
                if result is not False:return {'yes':True,'no':False}.get(result)
            for _ in range(3):
                try:answer=self._input(self.text('picture_prompt')).strip().casefold()
                except (EOFError,KeyboardInterrupt):return None
                if answer in ('y','yes','j','ja'):return True
                if answer in ('','n','no','nein'):return False
                if answer in ('c','cancel','a','abbrechen','q','exit'):return None
                self.say('choice_invalid')
            return None

    def confirm_uninstall(self):
        """A separate destructive confirmation, default Cancel on every console."""
        if not self.prompting:
            return False
        with self.suspend():
            self.say('confirm_uninstall')
            if self._arrow_capable():
                choices = [('cancel', self.text('confirm_cancel'), ''),
                           ('remove', self.text('confirm_remove'), '')]
                result = self._arrow_choose(choices, 'cancel', title='choice_confirm')
                if result is not False:
                    return result == 'remove'
            try:
                answer = self._input(self.text('confirm_remove_prompt')).strip()
            except (EOFError, KeyboardInterrupt):
                return False
            return answer.casefold() == self.text('confirm_remove_word').casefold()


@dataclass(frozen=True)
class Lan:
    address: str
    prefix: int
    interface: str
    gateway: str
    index: int = 0
    metric: int = 0

    @property
    def network(self):
        return ipaddress.ip_interface(self.address + '/' + str(self.prefix)).network


@dataclass(frozen=True)
class FoundFrame:
    host: str
    value: object
    origin: str


def private_ip(value):
    if type(value) is not str:
        return None
    try:
        address = ipaddress.ip_address(value)
    except (ValueError, TypeError):
        return None
    return address if address.version == 4 and any(address in network for network in PRIVATE) else None


def eligible_lan(lan):
    try:
        return (isinstance(lan, Lan) and type(lan.prefix) is int and 8 <= lan.prefix <= 30 and
                type(lan.interface) is str and re.fullmatch(r'[A-Za-z0-9 _.:()/-]{1,128}', lan.interface) is not None and
                not VIRTUAL.search(lan.interface) and private_ip(lan.address) is not None and
                private_ip(lan.gateway) is not None and ipaddress.ip_address(lan.gateway) in lan.network and
                any(lan.network.subnet_of(network) for network in PRIVATE))
    except (ValueError, TypeError):
        return False


def candidate_ip(value, lan=None):
    address = private_ip(value)
    if address is None:
        return None
    if lan is not None:
        if not eligible_lan(lan) or address not in lan.network or str(address) in (lan.address, lan.gateway):
            return None
        if address in (lan.network.network_address, lan.network.broadcast_address):
            return None
    return str(address)


def command_output(argv, *, timeout=3, limit=65536):
    """Bound/reap a fixed read-only OS command without background reader threads."""
    try:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, bufsize=0)
    except OSError:
        return b''
    result = bytearray()
    okay = True
    deadline = time.monotonic() + timeout
    try:
        if os.name == 'nt':
            import msvcrt
            from ctypes import wintypes
            peek = ctypes.WinDLL('kernel32', use_last_error=True).PeekNamedPipe
            peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                             wintypes.LPVOID, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
            peek.restype = wintypes.BOOL
            handle = msvcrt.get_osfhandle(child.stdout.fileno())
        while True:
            if time.monotonic() >= deadline:
                okay = False
                break
            if os.name == 'nt':
                available = wintypes.DWORD()
                if not peek(handle, None, 0, None, ctypes.byref(available), None):
                    break  # Broken pipe after process exit is normal EOF.
                if not available.value:
                    if child.poll() is not None:
                        break
                    time.sleep(.02)
                    continue
                count = min(4096, available.value)
            else:
                ready, _, _ = select.select([child.stdout], [], [], min(.05, max(0, deadline - time.monotonic())))
                if not ready:
                    continue
                count = 4096
            chunk = os.read(child.stdout.fileno(), count)
            if not chunk:
                break
            if len(result) + len(chunk) > limit:
                okay = False
                break
            result.extend(chunk)
    except (OSError, ValueError):
        okay = False
    finally:
        if not okay and child.poll() is None:
            child.kill()
        try:
            child.wait(timeout=max(.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
            okay = False
        child.stdout.close()
    return bytes(result) if okay and child.returncode == 0 else b''


def json_rows(raw, maximum=64):
    if not raw or len(raw) > 65536:
        return []
    try:
        value = json.loads(raw.decode('utf-8-sig'))
    except (ValueError, UnicodeError):
        return []
    rows = value if type(value) is list else [value]
    return rows if len(rows) <= maximum and all(type(row) is dict for row in rows) else []


def windows_directory():
    try:
        from ctypes import wintypes
        query = ctypes.WinDLL('kernel32', use_last_error=True).GetSystemDirectoryW
        query.argtypes = [wintypes.LPWSTR, wintypes.UINT]
        query.restype = wintypes.UINT
        buffer = ctypes.create_unicode_buffer(32768)
        size = query(buffer, len(buffer))
        return buffer.value if 0 < size < len(buffer) else None
    except (AttributeError, OSError):
        return None


WINDOWS_LAN = r"""[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false; @(Get-NetIPConfiguration | Where-Object { $_.NetAdapter.Status -eq 'Up' -and $_.NetAdapter.HardwareInterface -and -not $_.NetAdapter.Virtual -and $_.IPv4DefaultGateway } | Select-Object -First 8 | ForEach-Object { $c=$_; @($c.IPv4Address) | Select-Object -First 4 | ForEach-Object { [pscustomobject]@{address=$_.IPAddress;prefix=$_.PrefixLength;interface=$c.InterfaceAlias;index=$c.InterfaceIndex;gateway=$c.IPv4DefaultGateway.NextHop;metric=$c.NetIPv4Interface.InterfaceMetric;physical=$c.NetAdapter.HardwareInterface;virtual=$c.NetAdapter.Virtual} } }) | ConvertTo-Json -Compress"""
WINDOWS_NEIGHBORS = r"""[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false; @(Get-NetNeighbor -AddressFamily IPv4 | Where-Object { $_.State -in @('Reachable','Stale','Permanent') } | Select-Object -First 64 | ForEach-Object { [pscustomobject]@{address=$_.IPAddress;index=$_.InterfaceIndex} }) | ConvertTo-Json -Compress"""
WINDOWS_DNS = r"""[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false; @(Get-DnsClientCache | Where-Object { $_.Type -eq 1 -and $_.Entry -match '(^frame[.]local$|steam[-_]?frame)' } | Select-Object -First 32 | ForEach-Object { [pscustomobject]@{address=$_.Data;name=$_.Entry} }) | ConvertTo-Json -Compress"""


def _dns_name(data, offset):
    labels = []
    end = None
    seen = set()
    for _ in range(32):
        if offset >= len(data) or offset in seen:
            raise ValueError('DNS name bounds')
        seen.add(offset)
        length = data[offset]
        if length & 0xc0 == 0xc0:
            if offset + 1 >= len(data):
                raise ValueError('DNS pointer bounds')
            if end is None:
                end = offset + 2
            offset = ((length & 0x3f) << 8) | data[offset + 1]
            continue
        if length > 63 or offset + 1 + length > len(data):
            raise ValueError('DNS label bounds')
        offset += 1
        if not length:
            return '.'.join(labels).lower(), end if end is not None else offset
        labels.append(data[offset:offset + length].decode('ascii').lower())
        if sum(map(len, labels)) + len(labels) > 253:
            raise ValueError('DNS name bound')
        offset += length
    raise ValueError('DNS pointer cycle')


def mdns_addresses(data, names):
    """Read only requested A records. Name/port is still not Frame identity."""
    if len(data) < 12 or len(data) > 4096:
        return []
    _, flags, questions, answers, authorities, additional = struct.unpack_from('!6H', data)
    if not flags & 0x8000 or questions > 8 or answers + authorities + additional > 64:
        return []
    result = []
    try:
        offset = 12
        for _ in range(questions):
            _, offset = _dns_name(data, offset)
            offset += 4
            if offset > len(data):
                return []
        for _ in range(answers + authorities + additional):
            name, offset = _dns_name(data, offset)
            if offset + 10 > len(data):
                return []
            kind, klass, _, count = struct.unpack_from('!HHIH', data, offset)
            offset += 10
            if offset + count > len(data):
                return []
            if name in names and kind == 1 and klass & 0x7fff == 1 and count == 4:
                value = str(ipaddress.ip_address(data[offset:offset + count]))
                if private_ip(value) is not None:
                    result.append(value)
            offset += count
    except (ValueError, UnicodeError, struct.error):
        return []
    return list(dict.fromkeys(result))


def mdns(lan, names=NAMES):
    if not eligible_lan(lan):
        return []
    result = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.bind((lan.address, 0))
            client.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(lan.address))
            client.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            client.settimeout(.15)
            for name in names[:4]:
                question = b''.join(bytes([len(part)]) + part.encode('ascii') for part in name.split('.')) + b'\x00'
                client.sendto(struct.pack('!6H', 0, 0, 1, 0, 0, 0) + question + struct.pack('!HH', 1, 0x8001), ('224.0.0.251', 5353))
            end = time.monotonic() + .65
            for _ in range(32):
                if time.monotonic() >= end:
                    break
                try:
                    packet, _ = client.recvfrom(4096)
                except socket.timeout:
                    continue
                result.extend(mdns_addresses(packet, names))
    except OSError:
        return []
    return list(dict.fromkeys(value for value in result if candidate_ip(value, lan)))


class SystemSources:
    """Fixed OS commands; only a proven physical active private LAN may sweep."""
    def __init__(self, *, system=None, run=command_output, system_directory=None,
                 device_exists=None, multicast=mdns):
        self.system = platform.system() if system is None else system
        self.run = run
        self.directory = windows_directory() if self.system == 'Windows' and system_directory is None else system_directory
        self.device_exists = device_exists or (lambda name: (Path('/sys/class/net') / name / 'device').exists())
        self.multicast = multicast
        self.ip_tool = shutil.which('ip')
        self._resolved = {}

    def reset_discovery(self):
        """Discard DNS hints between explicitly requested bounded searches."""
        self._resolved.clear()

    def _powershell(self, command):
        if not self.directory:
            return []
        path = ntpath.join(self.directory, 'WindowsPowerShell', 'v1.0', 'powershell.exe')
        return json_rows(self.run([path, '-NoLogo', '-NoProfile', '-NonInteractive', '-Command', command]))

    def lan(self):
        choices = []
        if self.system == 'Windows':
            for row in self._powershell(WINDOWS_LAN):
                if row.get('physical') is not True or row.get('virtual') is not False:
                    continue
                try:
                    lan = Lan(row['address'], row['prefix'], row['interface'], row['gateway'], row['index'], row.get('metric') or 0)
                    if eligible_lan(lan) and type(lan.index) is int and type(lan.metric) is int:
                        choices.append(lan)
                except (KeyError, TypeError, ValueError):
                    continue
        elif self.system == 'Linux' and self.ip_tool:
            routes = json_rows(self.run([self.ip_tool, '-j', '-4', 'route', 'show', 'default']))
            for route in routes[:8]:
                name = route.get('dev', '')
                if type(name) is not str or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,32}', name) or VIRTUAL.search(name):
                    continue
                if not self.device_exists(name):
                    continue
                rows = json_rows(self.run([self.ip_tool, '-j', '-d', '-4', 'address', 'show', 'dev', name, 'up']))
                for row in rows:
                    if row.get('link_type') != 'ether' or type(row.get('linkinfo', {})) is not dict or row.get('linkinfo', {}).get('info_kind'):
                        continue
                    addresses = row.get('addr_info', [])
                    if type(addresses) is not list:
                        continue
                    for address in addresses[:8]:
                        try:
                            lan = Lan(address['local'], address['prefixlen'], name, route['gateway'], row['ifindex'], route.get('metric', 0))
                            if address.get('family') == 'inet' and eligible_lan(lan) and type(lan.index) is int and type(lan.metric) is int:
                                choices.append(lan)
                        except (KeyError, TypeError, ValueError):
                            continue
        return min(choices, key=lambda item: item.metric) if choices else None

    def resolve(self, name):
        # OS DNS/mDNS support is useful even when physical-LAN inventory is
        # unavailable. Run it in a bounded/reaped child: getaddrinfo has no
        # portable per-call deadline, and daemon threads would outlive a check.
        if type(name) is not str or len(name) > 253 or not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?', name):
            return []
        if name not in self._resolved:
            code = ('import socket,json,sys; '
                    'print(json.dumps(sorted({r[4][0] for r in '
                    'socket.getaddrinfo(sys.argv[1],22,socket.AF_INET,socket.SOCK_STREAM)})))')
            raw = self.run([sys.executable, '-I', '-c', code, name], timeout=1.25, limit=2048)
            try:
                values = json.loads(raw.decode('utf-8'))
            except (ValueError, UnicodeError):
                values = []
            self._resolved[name] = list(dict.fromkeys(value for value in values
                if type(value) is str and candidate_ip(value)))[:8] if type(values) is list else []
        return self._resolved[name][:]

    def named(self, lan):
        values = []
        for name in NAMES:
            values.extend(self.resolve(name))
        if eligible_lan(lan):
            values.extend(self.multicast(lan, NAMES))
        if self.system == 'Windows':
            for row in self._powershell(WINDOWS_DNS):
                if type(row.get('name')) is str and re.search(r'(?i)(^frame[.]local$|steam[-_]?frame)', row['name']):
                    values.append(row.get('address'))
        return list(dict.fromkeys(value for value in values if candidate_ip(value, lan)))[:8]

    def neighbors(self, lan):
        if not eligible_lan(lan):
            return []
        if self.system == 'Windows':
            rows = self._powershell(WINDOWS_NEIGHBORS)
            values = [row.get('address') for row in rows if row.get('index') == lan.index]
        elif self.system == 'Linux' and self.ip_tool:
            rows = json_rows(self.run([self.ip_tool, '-j', '-4', 'neigh', 'show', 'dev', lan.interface]), MAX_NEIGHBORS)
            values = [row.get('dst') for row in rows if type(row.get('state', [])) in (list, str) and not any(value in row.get('state', []) for value in ('FAILED', 'INCOMPLETE'))]
        else:
            values = []
        return list(dict.fromkeys(value for value in values if candidate_ip(value, lan)))[:MAX_NEIGHBORS]


def port_open(address, port=22, timeout=.15):
    try:
        with socket.create_connection((address, port), timeout=timeout):
            return True
    except OSError:
        return False


def scan(lan, connector=port_open):
    if not eligible_lan(lan):
        return []
    network = ipaddress.ip_network(lan.address + '/' + str(max(lan.prefix, 24)), strict=False)
    addresses = [str(address) for address in network.hosts() if candidate_ip(str(address), lan)][:MAX_SCAN_HOSTS]
    def check(address):
        try:
            return address if connector(address, 22, .15) else None
        except OSError:
            return None
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        return [value for value in pool.map(check, addresses) if value]


def find_frame(validate, *, cached_hosts=(), sources=None, presenter=None,
               allow_manual=True, connector=port_open):
    """Run one bounded, read-only search per explicit user request.

    validate(host, interactive=False) MUST use batch authentication/no prompts.
    Each pass has at most eight batch validations and one strong-hint login;
    a retry gets a fresh budget and fresh DNS hints. Unknown SSH hosts never
    trigger login prompts automatically. Manual IP entry and another search
    require explicit choices. Check-only returns None; Exit/cancel propagates
    KeyboardInterrupt so a caller cannot mistake cancellation for PC-only mode.
    """
    ui = presenter or Presenter()
    sources = sources or SystemSources()
    # Cache inputs are hints, not a source that can grow between retry passes.
    # Bound iterator consumption as well as the number of network validations.
    import itertools
    cached_hosts = tuple(itertools.islice(cached_hosts, 4))

    def validate_one(address, origin, interactive=False):
        try:
            # The native Textual authentication broker owns its password/host-key
            # widgets. Only the plain terminal fallback borrows OpenSSH's tty.
            scope = ui.suspend() if interactive and not getattr(ui, 'inline_ssh', False) else nullcontext()
            with scope:
                value = validate(address, interactive=interactive)
        except (OSError, ValueError):
            # Never echo arbitrary SSH diagnostics or secrets. A failed manual
            # check gets the same explicit Retry/Manual/PC-only/Exit choices.
            return None
        if value is not None and value is not False:
            ui.say('found')
            return FoundFrame(address, value, origin)
        return None

    def search_once():
        seen, attempts, interactive_hint, lan = set(), 0, None, None

        def candidates(values, origin, strong=False):
            nonlocal attempts, interactive_hint
            limit = {'cached': 32, 'named': 8, 'neighbor': MAX_NEIGHBORS, 'local-scan': MAX_SCAN_HOSTS}[origin]
            for address in itertools.islice(values, limit):
                address = candidate_ip(address, lan)
                if not address or address in seen:
                    continue
                seen.add(address)
                if attempts >= MAX_VALIDATIONS:
                    break
                try:
                    reachable = connector(address, 22, .15)
                except OSError:
                    reachable = False
                if not reachable:
                    continue
                attempts += 1
                found = validate_one(address, origin)
                if found:
                    return found
                if strong and interactive_hint is None:
                    interactive_hint = (address, origin)
            return None

        with ui.progress('search'):
            cached = []
            for hint in cached_hosts:
                if candidate_ip(hint):
                    cached.append(hint)
                else:
                    cached.extend(itertools.islice(sources.resolve(hint), 8))
            found = candidates(cached, 'cached', True)
            if found:
                return found
            # DNS hints remain useful when physical-interface inventory is
            # unavailable. A hint never substitutes for real Frame identity.
            found = candidates(sources.named(None), 'named', True)
            if found:
                return found
            lan = sources.lan()
            if not eligible_lan(lan):
                lan = None
            if lan is not None:
                found = candidates(sources.named(lan), 'named', True)
                if found:
                    return found
            if interactive_hint and ui.prompting:
                ui.say('login')
                found = validate_one(*interactive_hint, interactive=True)
                if found:
                    return found
        if lan is not None:
            with ui.progress('nearby'):
                found = candidates(sources.neighbors(lan), 'neighbor')
                if found:
                    return found
                if attempts < MAX_VALIDATIONS:
                    found = candidates(scan(lan, connector), 'local-scan')
                    if found:
                        return found
        return None

    while True:
        try:
            found = search_once()
        except (OSError, ValueError):
            found = None
        if found:
            return found
        ui.say('missing')
        if not ui.prompting:
            ui.say('pc_only')
            return None
        actions = ['retry', 'manual', 'check', 'exit'] if allow_manual else ['retry', 'check', 'exit']
        while True:
            action = ui.choose_action(actions, recommended='retry')
            if action == 'retry':
                reset = getattr(sources, 'reset_discovery', None)
                if callable(reset):
                    reset()
                break
            if action == 'check':
                ui.say('pc_only')
                return None
            if action != 'manual':
                raise KeyboardInterrupt()
            value = ui.ask('manual').strip()
            if not value:
                continue  # Back means another explicit choice, never PC-only.
            address = candidate_ip(value)
            if address is None:
                ui.say('invalid')
                continue
            found = validate_one(address, 'manual', interactive=True)
            if found:
                return found
            ui.say('unverified')
