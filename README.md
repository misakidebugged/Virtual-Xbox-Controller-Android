# Generic Bluetooth Controller to Virtual Xbox 360 Controller

![Android Min SDK](https://img.shields.io/badge/Android-8.0%2B-3DDC84?style=for-the-badge&logo=android&logoColor=white)
![Shizuku Required](https://img.shields.io/badge/Shizuku-Required-FF6F00?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-blue?style=for-the-badge)

An Android application that turns generic HID Bluetooth gamepads (and Valve Steam Controllers) into a system-wide recognized official Xbox 360 controller using elevated rootless permissions via [Shizuku](https://shizuku.rikka.app/). 

This project is a specialized fork of [SonicDX12/SteamController-Android](https://github.com/SonicDX12/SteamController-Android), expanded to support generic Bluetooth HID controllers as its primary focus.

---

## Why This Project Exists

Many high-end Android mobile games boast official gamepad support, yet routinely ignore budget gamers who cannot or do not want to spend $50–$80 on an official Xbox or PlayStation controller. Meanwhile, perfectly usable $20–$30 Bluetooth gamepads (such as iPega, 8BitDo, and generic HID controllers) are left unrecognized by major titles:

- **Call of Duty: Mobile**
- **Apex Legends Mobile**
- **Alien: Isolation**
- **GRID Autosport**
- **GTA: The Trilogy – The Definitive Edition**
- **Dead Cells**
- **Castlevania: Symphony of the Night**
- **Pascal's Wager**
- **Bloodstained: Ritual of the Night**
- **Minecraft**
- **Genshin Impact**
- **Arknights: Endfield**
- ...and many, many more.

> *"I wanted a solution that would route my existing controller to be recognized as an Xbox 360 controller system-wide on Android—similar to the popular Windows tool **x360ce**—but couldn't find one..."*
>
> ### ***"Fine, I'll do it myself."***

---

> [!NOTE]
> **Tested & Verified Hardware**: Real-world testing was conducted on an **iPega extendable HID controller**, successfully enabling native controller support in **Arknights: Endfield** via the virtual Xbox 360 controller.

---

## How It Works

```
Physical Bluetooth / USB Controller (iPega, 8BitDo, HID, Steam Controller)
                          │
                          ▼
            Linux evdev (/dev/input/event*)
                          │
                          ▼
             Exclusive Input Grab (EVIOCGRAB)
            (Prevents double/ghost keypresses)
                          │
                          ▼
     ControllerService & Motion/Button Mapping Pipeline
                          │
                          ▼
          UInputService (via Shizuku / shell UID)
                          │
                          ▼
             Linux /dev/uinput Kernel Device
                          │
                          ▼
       System-Wide Virtual Xbox 360 Controller
              (Recognized natively by Android & games)
```

1. **Input Interception (`evdev`)**: Intercepts low-level input events directly from `/dev/input/event*` nodes for connected Bluetooth/USB controllers.
2. **Exclusive Input Grabbing (`EVIOCGRAB`)**: Grabs the physical controller exclusively at the Linux kernel level while active, preventing double-keypresses or ghost inputs in games.
3. **Virtual Gamepad Creation (`/dev/uinput`)**: Spawns a real, system-wide virtual Xbox 360 controller via Shizuku (`shell` UID).
4. **Game Recognition**: Games recognize a native Xbox 360 controller with standard button prompts and full compatibility.

---

## Features

### Connection & Routing
- **Generic Bluetooth HID Controllers**: Full support for any paired Bluetooth gamepad (iPega, 8BitDo, MOGA, generic HID controllers).
- **Steam Controller Support**: Supports Valve Steam Controllers (2026 models) via Bluetooth LE or USB OTG (Puck dongle).
- **Exclusive Access**: Employs Linux `EVIOCGRAB` to prevent physical inputs from leaking alongside virtual inputs.
- **Auto-Start**: Automatically launches the controller service when a paired Bluetooth controller connects.

### Emulation & Profiles
- **Emulated Gamepads**: Xbox 360 (default for broadest game compatibility), Xbox One, DualShock 4, DualSense, and Switch Pro.
- **Desktop Mode**: Maps controller inputs to virtual mouse movements and keyboard keys for Android TV and app navigation.
- **Game Profile Auto-Switching**: Automatically switches mappings and profiles based on the foreground application.
- **Notification Quick-Switch**: Cycle active emulated controller profiles directly from the status notification.

### Tuning & Customization
- **Stick Calibration**: Live dead zone tuning, center offset capture, Y-axis inversion, and 2D visualizer.
- **Button Remapping**: Remap physical buttons to Xbox/PlayStation targets, keyboard keys, or system actions (such as taking screenshots).
- **Rumble Support**: Forward vibration rumble effects to supported controllers (Steam Controllers) with adjustable intensity.

---

## Requirements

- **Android 8.0+** (API 26) — Phone, Tablet, or Android TV.
- **[Shizuku](https://shizuku.rikka.app/)** installed and running (via Wireless ADB or one-time PC ADB connection).
- Any paired **Bluetooth Controller** or **Steam Controller** (2026 Models Only) (USB OTG / BLE).

---

## Setup & Quick Start

1. Install and start [Shizuku](https://shizuku.rikka.app/).
2. Pair your Bluetooth gamepad in Android System Settings.
3. Open this app and grant Shizuku and Notification permissions when prompted.
4. Select **Bluetooth** on the Home tab, pick your controller (or **Any Connected Bluetooth Controller**), and choose your emulated controller profile (e.g. **Xbox 360**).
5. Tap **Start Service**. The physical controller inputs will be grabbed exclusively and translated into your virtual Xbox 360 gamepad.
6. Launch your game and enjoy native controller gameplay!

---

## Roadmap & TODO

- [x] **Bluetooth Detection**: Make Bluetooth peripherals discoverable and selectable in-app.
- [x] **Generic HID Controller Support**: Route generic HID controllers to a system-wide virtual Xbox 360 gamepad.
- [ ] **Advanced In-App Button & Axis Remapping**: Add an interactive manual mapping UI for controllers with non-standard physical button layouts or non-standard stick axis assignments.
- [ ] **Bluetooth Device Filtering**: Filter the paired Bluetooth device list to display only gamepad/controller class peripherals.
- [ ] **Gyro Aiming**: Route raw gyroscope data to DualShock 4 / DualSense motion controls for supported games.

---

## Credits

- **[Shizuku](https://github.com/RikkaApps/Shizuku)** by RikkaApps — Provides rootless access to `/dev/uinput` via the `shell` UID.
- **[SonicDX12/SteamController-Android](https://github.com/SonicDX12/SteamController-Android)** — The original repository from which this project was forked. Credit to SonicDX12 and contributors for developing the foundational Steam Controller driver, uinput service pipeline, and application architecture.
- **Development & Testing**: Android Studio using Gemini 3.6 Flash for configuration and controller mappings (JNI/Evdev architecture).
- **SteamlessController** by ddeverill & Linux `hid-steam` driver — Reference material for Valve Steam Controller HID report parsing.

---

## License

MIT — See [LICENSE](LICENSE).
