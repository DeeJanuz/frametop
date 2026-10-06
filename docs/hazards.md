# Potential hazards

Known ways the input changes can go wrong, and what to check when something looks off. Each has been reasoned through but not all have been seen on a headset. [design.md](design.md) explains why the input relay works the way it does.

## Volume keys

The input relay takes the volume keys from every device that has them, so gamescope never sees one (a volume press with nothing focused aborts gamescope and ends the VR session). On devices with a keymap it remaps the volume entries to stand-in codes (`KEY_MACRO29`, `KEY_MACRO30`), and it grabs `pmic_resin`.

- **Keymaps stay remapped if the relay dies.** The keymaps go back when the relay exits normally or on `systemctl stop` (SIGTERM). A crash or SIGKILL skips that, and until the relay starts again the volume keys do nothing, on the headset and on any keyboard it remapped. The headset's own buttons don't reconnect, so for them only the relay coming back (or a reboot) fixes it. The relay recognizes the stand-in codes on start and takes them over again.
- **The headset's other buttons share the device.** `gpio-keys` carries the click button as well as volume. Only the volume entries are remapped, but if the click button stops working, check this first (`--no-grab` leaves the volume keys alone).
- **The relay opens more devices than it used to.** It now opens any device with volume keys, whatever its bus, not only USB and Bluetooth mice and keyboards. A device it can't remap and that has more than volume keys is left alone, and its volume keys still reach gamescope (the log says "can't take over its volume keys").
- **Volume goes to the default output.** `wpctl` steps `@DEFAULT_AUDIO_SINK@` by 5%, capped at 100%. If sound plays somewhere other than the default output, the keys change the wrong one. Steam never sees the keys, so anything it did on a volume press no longer happens.
- **Repeat is the relay's own.** Holding a key repeats after 0.4 s, every 0.1 s, and kernel autorepeat from keyboards is ignored. If a device disconnects mid-hold, the repeat stops with it.

## Key releases

ft-screens drops keys while no screen has focus or the SteamVR dashboard is open, but always lets through the release of a key the desktop saw pressed, so a modifier held as the dashboard opens doesn't stay down.

- **A key whose release never arrives stays held in the desktop until the relay clears it, within about a second.** KWin repeats held keys itself, so a stuck letter repeats and a stuck modifier changes every later key (Ctrl+Alt held turns T into Konsole). The relay remembers which keys it told the desktop went down, and once a second it releases any that no keyboard holds (`reconcile_desktop_keys`, which asks the kernel with `EVIOCGKEY`). Pressing and releasing the key again also clears it.
- **A keyboard that disconnects mid-press, or a relay restart with a key down, is how it happens.** The once-a-second check catches the first. A relay that starts doesn't know what an earlier one left down, so it releases the modifiers and mouse buttons on the desktop; another key left down that way stays until it's pressed and released again.
- **To see where a key went,** run `scripts/keys-report.py` and reproduce the problem while it records. It logs the modifiers, Tab, and Esc (no other keys) as the relay reads them and as its virtual keyboard sends them on, with the device roles and grabs, which programs have each keyboard open, and the relay's and desktop's logs.
- **Switching where typing goes waits for keys to come up.** The relay changes a keyboard's grab only while none of its keys are down, so a press and its release go to the same side. A key held for a long time delays the switch until it's let go.
- **Mouse buttons passed through as keys follow the pointer, not typing.** In pointer mode, a mouse button set to Pass through as key (a side button for Back) goes to the screen under the pointer, only while there is one and Frametop isn't paused. Its release always goes through, and ft-screens releases it itself when the pointer leaves the screens, its screen hides or closes, or Frametop pauses; the real release that comes later is dropped. So a drag held with it ends as the laser leaves a screen, unlike a laser's own click, which KWin keeps until it comes up. wlroots counts presses per button, so a release lost on the way would leave that button dead in the desktop, the lasers' clicks included, until ft-screens restarts. The button also goes to the virtual mouse, which gamescope reads, so the app Steam has focused may see it too.
- **Media keys reach both sides.** A keyboard's media keys come from its Consumer Control node, which has volume keys too, so the relay remaps that node rather than grabbing it. Its other keys reach gamescope as well as the desktop, even while typing goes to the desktop and the keyboard itself is grabbed: Play/Pause on the desktop can pause something in Steam too. The headset's own buttons don't go to the desktop.

## Typing and grabbed keyboards

- **Programs that watch every keyboard lose grabbed ones.** While typing goes to the desktop, the relay grabs pass-through keyboards, so a hotkey tool reading them directly stops seeing their keys. `SHARE_KEYS=1` in `~/.config/frametop.conf` sends their keys to the abstract socket `@frametop_keys` instead. It's off by default: abstract sockets have no permissions, and any local process that binds the name first receives every key typed into the desktop, passwords included.
- **Typing starts out going to Steam.** After the desktop starts, keys go to Steam until you click a screen.
- **Controller clicks don't move typing.** Overlay apps don't see controller clicks on other panels, so after one typing stays where it was. A mouse click, or a click on a screen, moves it.
