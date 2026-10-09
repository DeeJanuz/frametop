# setup

One-time setup for the Steam Frame itself: the `dev` build container and the Bluetooth fixes. `install.sh` runs both, so you only need this page for details, doing a step on its own, or troubleshooting. Run the scripts from the repo root, either in a terminal on the Frame (the usual way) or from a PC over SSH (see "Developing from a PC" in the README).

| Script | What it sets up | Needs host `sudo` |
| --- | --- | --- |
| `dev-container.sh` | The `dev` build container (Fedora 44 toolbox) with every package the projects need | No |
| `bluetooth/install.sh` | Persistent fixes so Bluetooth LE mice and keyboards reconnect | Yes |

## Build container

```
setup/dev-container.sh
```

It creates the `dev` distrobox if it's missing and installs the packages listed in the script, which is the source of truth for the container. It also links `/opt/steamvr` to the host's SteamVR, so OpenVR programs built there can find the runtime. It's safe to re-run, for example after adding a package to the list.

## Bluetooth LE mice and keyboards

### Why this is needed

On stock SteamOS 0.3.0, Bluetooth LE mice and keyboards that use private addresses, such as the Swiftpoint Z3, pair but never reconnect. After the device sleeps or the Frame reboots, it stays disconnected. Two separate problems cause this:

1. BlueZ 5.79 never sets the `ADDRESS_RESOLUTION` device flag. Without it, kernel 6.18 doesn't load the device's identity key into the Bluetooth controller, so the controller can't recognize the device's rotating private address and ignores it. This is fixed upstream in BlueZ commit `f1fb4f95f4` ("core: Fix not resolving addresses"), but SteamOS doesn't ship that fix yet.
2. Some devices only know the Frame's public address. The Z3 doesn't accept the Frame's identity key during pairing, so it can only reconnect to the Frame's fixed public address. SteamOS sets `Privacy = device` in `/etc/bluetooth/main.conf`, and bluetoothd turns privacy back on at every start.

Both settings reset whenever bluetoothd restarts or the Frame reboots, so they have to be reapplied every time. That's what this setup installs.

### What gets installed

| File on the Frame | Purpose |
| --- | --- |
| `/etc/steamframe/bt-fixups.sh` | Turns controller privacy off, then sets the `ADDRESS_RESOLUTION` flags (`0x6`) on every bonded LE device that has an identity key |
| `/etc/systemd/system/steamframe-bt-fixups.service` | Runs the script after every Bluetooth start |
| `/etc/atomic-update.conf.d/frametop-bluetooth.conf` | Keeps the script through SteamOS updates |

The service runs after Bluetooth has started and never makes Bluetooth wait for it, because SteamOS's `set-bluetooth-mac-address.service`, which gives the Bluetooth chip its address, needs `bluetooth.service` to finish starting first. SteamOS's own files, including `main.conf`, are left untouched, so system updates won't conflict.

### Step 1: have a password for sudo

The install writes to `/etc`, so it needs `sudo` and the `steamos` user's password.

- On the headset, `sudo` asks for the password in the terminal. If you've never set one, run `passwd` first.
- From a PC over SSH, the scripts ask for it in your terminal (through `ssh -t`). To skip the question, or with no terminal, put the password in a `.env` file at the repo root:

  ```
  steamos_root_pwd="your-password"
  ```

  `.env` is gitignored and never synced to the Frame. The scripts send the password to `sudo` on stdin, never on a command line.

### Step 2: install the fixes

```
setup/bluetooth/install.sh install
```

It copies the files above into place and enables the service. A SteamOS update deletes every `/etc` file that SteamOS's keep list (`/usr/lib/rauc/atomic-update-keep.conf`) doesn't name. That list keeps the service but not the script, so the third file adds the script to it. `./install.sh` offers this same step.

### Step 3: pair your mouse or keyboard

Pair it the normal way, in Steam under Settings → Bluetooth. Then apply the fixes to the new device:

```
setup/bluetooth/install.sh run
```

or use Apply Bluetooth fixes on the Bluetooth page of Frametop Input Settings, which asks for the password. Do this once per newly paired device. From then on, the service applies the fixes automatically at every boot.

If a device won't pair at all, turn the fixes on first (`run`), then pair again. With privacy on, some devices, the Z3 included, fail to finish connecting.

### Step 4: check it worked

In a terminal on the Frame (or over `ssh frame`):

```
systemctl status steamframe-bt-fixups.service      # active (exited)
journalctl -b -u steamframe-bt-fixups --no-pager   # "privacy turned off" (if it was on), then
                                                   # "Set device flag of <address> (LE Random)" per device
sleep 2 | btmgmt info | grep "current settings"    # the list must NOT contain "privacy"
bluetoothctl info <address> | grep Connected       # "yes" once the device is awake
```

The real test is to reboot the Frame, then move or click the device. It should reconnect within a few seconds, with no re-pairing.

### Troubleshooting

#### The device paired but won't reconnect

Run `setup/bluetooth/install.sh run` again, then wake the device. Check that its address appears in the service log. The script only flags devices that have an identity key: look for `[IdentityResolvingKey]` in `/var/lib/bluetooth/<controller>/<device>/info` (readable as root).

#### Devices stopped reconnecting after a SteamOS update

Fixes installed before the keep entry existed lose their script in the update, and the service fails with `Unable to locate executable '/etc/steamframe/bt-fixups.sh'`. `scripts/doctor.sh` reports it too. Install again, which adds the keep entry, then start the service:

```
setup/bluetooth/install.sh install
sudo systemctl restart steamframe-bt-fixups.service
```

The update keeps a copy of the deleted files in `/etc/previous`.

#### No Bluetooth at all after a boot

Check the controller:

```
hciconfig hci0 | head -3    # healthy: "BD Address: 90:82:C3:..." and "UP RUNNING"
```

If it shows `DOWN RAW` with address `00:00:00:00:5A:AD`, the address service failed. Start it by hand, then restart the fixes:

```
sudo systemctl start set-bluetooth-mac-address.service
sudo systemctl restart steamframe-bt-fixups.service
```

This happened once, while an earlier version of the fix made Bluetooth wait on it. Never add an `ExecStartPost` or anything else that holds up `bluetooth.service`.

#### The mouse connects but does nothing in VR

That's not Bluetooth. SteamVR only reads input devices that existed when it started. Frametop's input relay handles this with permanent virtual devices (see `docs/reference.md`).

### Uninstall

```
setup/bluetooth/install.sh uninstall
```

It removes both files and disables the service. Privacy returns to SteamOS's default at the next Bluetooth restart or reboot.

### Verified on

SteamOS 0.3.0 (build `20260922.6101926`), BlueZ 5.79, kernel 6.18, Qualcomm WCN7850 (`hci0`), with a Swiftpoint Z3, including cold-boot reconnects (2026-09-25).
