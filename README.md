# OpenBridge – Digitone II

Linux implementation of Elektron Overbridge for the **Elektron Digitone II**:

![OpenBridge Digitone II, track page](docs/screenshot.png)

## Features

- **Multichannel USB audio**: Main, Track 1–16, FX and Ext-In outputs (42 channels) and the 8
  inputs, with clock-drift compensation between the DAW and the device.
- **Every sound parameter** of the 16 tracks (FM Tone, FM Drum, WaveTone, Swarmer, MIDI machines;
  all filter machines), read from the device and editable, with DAW automation.
- **Editor**: machine pages, filter, amp, LFOs, modulation (key tracking,
  pitch bend, velocity, mod wheel, breath, aftertouch), SETUP (TRK, VOICE, PORTAMENTO, LAYER),
  send FX, mixer with level meters, compressor, Ext-In, on-screen keyboard.
- **Mixer**: track levels, mutes, pattern mutes, solo, pan, send FX, master, compressor and
  compressor routing, read from the device when the plugin opens. **Audio to:** sends each channel
  to the DAW output of your choice (LR or 1–20).
- **Preset Browser**: kit track presets, project preset pool, +Drive library (banks A–H, tags,
  filters); load a +Drive preset into a track; Clear a track.
- **Changes made on the device** (knobs, mixer, mutes) are followed by the plugin.
- **Total Recall**: the device's whole project can be stored in the DAW project and restored when
  the DAW project is opened (see below).

## Requirements

- Linux x86_64, **Ubuntu 22.04 or later** (or a distribution with glibc 2.35 or later).
- An Elektron Digitone II on **OS 1.11 or later**. Older OS versions do not answer the mixer and FX messages.
- On the Digitone, **USB CONFIG** set to **OVERBRIDGE**.

## Installation

### .deb package (Ubuntu / Debian)

Download `openbridge-digitone2_0.1.7_amd64.deb` from the
[Releases](https://github.com/Donmanouche/OpenBridge-Digitone-2/releases) page, then:

```bash
sudo apt install ./openbridge-digitone2_0.1.7_amd64.deb
```

It installs:

- the VST3 plugin in `/usr/lib/vst3/OpenBridge Digitone II.vst3`;
- a udev rule that gives the logged-in user access to Elektron USB devices.

Unplug and replug the Digitone after installing.

### Archive

`openbridge-digitone2-0.1.7-linux-x86_64.tar.gz` holds the same files.

1. Copy `OpenBridge Digitone II.vst3` to `~/.vst3/`.
2. Install the udev rule:

   ```bash
   sudo cp 70-openbridge-elektron.rules /etc/udev/rules.d/
   ```

   ```bash
   sudo udevadm control --reload-rules
   ```

## Total Recall

Click **Total Recall** in the plugin's header:

- **ENABLE NOW** starts a session: the Digitone's current project is copied into the plugin and saved
  with the DAW project. **SYNC NOW** copies it again.
- **Sync automatically on save** (on by default): the project is copied again each time the DAW
  project is saved (up to 10 s; otherwise the previous copy is saved).
- When the DAW project is opened again, or the Digitone is reconnected, the plugin and the Digitone
  ask which state to use: **Plugin State** sends the saved project to the Digitone, **Device State**
  keeps the Digitone's current project (and copies it into the plugin), **Cancel** stops the session.
  The question can also be answered on the Digitone.
- **DISABLE** ends Total Recall and removes the copy from the DAW project.

Plugin State replaces the project currently loaded on the Digitone.

## Building from source

Dependencies (Ubuntu 22.04 package names):

```bash
sudo apt install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev libsamplerate0-dev libasound2-dev libjack-jackd2-dev libfreetype-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev
```

You also need **JUCE 7**, in one of two ways:

- **Packaged JUCE**: install the `juce-modules-source` package (Ubuntu 24.04 and later, Debian).
- **JUCE sources**: download [JUCE 7.0.5](https://github.com/juce-framework/JUCE/releases/tag/7.0.5)
  and pass its folder with `-DOB_JUCE_DIR=/path/to/JUCE`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
ninja -C build
```

The plugin is in `build/OpenBridgeDigitone2_artefacts/Release/VST3/`.


## Disclaimer

OpenBridge is an independent project. It is not affiliated with, endorsed or supported by Elektron.
Elektron, Overbridge and Digitone are trademarks of Elektron Music Machines MAV AB. Use it at your
own risk.

## License

GPL-3.0. See [LICENSE](LICENSE). Built with [JUCE](https://juce.com) under its GPL licence.
