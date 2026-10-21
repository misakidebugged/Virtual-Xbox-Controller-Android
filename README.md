# Universal Bluetooth Gamepad & Steam Controller for Android

> *"Fine, I'll do it myself."*

Modern Android mobile games boast official gamepad support, yet they routinely ignore budget gamers who cannot or do not want to spend $50–$80 on an official Xbox or PlayStation controller—especially expensive telescopic controllers designed for phone gaming. Meanwhile, perfectly usable $20–$30 Bluetooth gamepads (such as iPega, 8BitDo, and generic HID controllers) are left unrecognized.

This app is a specialized fork focused primarily on **turning any generic Bluetooth HID controller** (and Valve Steam Controllers) into a **native, system-wide Xbox 360 / Xbox One / PlayStation / Switch Pro gamepad** without root, powered by [Shizuku](https://shizuku.rikka.app/).

Real-world testing has been performed on an **iPega extendable HID controller**, successfully enabling native controller support in games such as **Arknights: Endfield** via the virtual Xbox 360 controller.

## How It Works

1. **Input Interception (`evdev`)**: Intercepts input events directly from Linux `/dev/input/event*` nodes for any connected Bluetooth or USB controller.
2. **Exclusive Input Grabbing (`EVIOCGRAB`)**: Grabs the physical controller exclusively at the Linux kernel level while the service is active, stopping double-keypresses and ghost inputs in games.
3. **Virtual Gamepad Creation (`/dev/uinput`)**: Creates a real system-wide virtual gamepad (Xbox 360, Xbox One, DualShock 4, DualSense, or Switch Pro) via Shizuku (`shell` UID).
4. **Game Recognition**: Android and mobile games recognize a standard, official Xbox/PlayStation controller, enabling native button prompts and full game compatibility.

## Features

### Connection & Devices
- **Generic Bluetooth HID Controllers**: Use any paired Bluetooth gamepad (iPega, 8BitDo, MOGA, generic HID pads, etc.).
- **Steam Controller**: Supports the Valve Steam Controller via Bluetooth LE or USB OTG (with wireless Puck dongle).
- **Exclusive Access**: Utilizes Linux `EVIOCGRAB` to prevent physical button events from leaking into Android alongside virtual events.
- **Auto-Start**: Automatically launches the controller service when a paired Bluetooth controller connects.

### Emulation & Profiles
- **Emulated Gamepads**: Xbox 360 (default for broadest game compatibility), Xbox One, DualShock 4, DualSense, and Switch Pro.
- **Desktop Mode**: Maps controller inputs to virtual mouse movements and keyboard keys, ideal for Android TV and app navigation.
- **Game Profile Auto-Switching**: Automatically switch profiles, calibration, and button mappings based on the foreground game being launched.
- **Notification Quick-Switch**: Cycle active emulated controller profiles directly from the persistent status notification.

### Tuning & Customization
- **Stick Calibration**: Live dead zone tuning, center offset capture, Y-axis inversion, and 2D visualizer.
- **Button Remapping**: Remap any physical button to Xbox/PlayStation targets, keyboard keys, or system actions (such as taking a screenshot).
- **Rumble Support**: Forward vibration rumble effects to supported controllers with adjustable intensity.

## Requirements

- **Android 8.0+** (API 26) — Phone, Tablet, or Android TV.
- **[Shizuku](https://shizuku.rikka.app/)** installed and running (via Wireless ADB on Android 11+ or a one-time PC ADB connection).
- Any paired **Bluetooth Controller** or **Steam Controller** (USB OTG / BLE).

## Setup & Quick Start

1. Install and start [Shizuku](https://shizuku.rikka.app/).
2. Pair your Bluetooth gamepad in Android System Settings.
3. Open this app and grant Shizuku and Notification permissions when prompted.
4. Select **Bluetooth** on the Home tab, pick your controller (or **Any Connected Bluetooth Controller**), and select your desired emulated controller profile (e.g. **Xbox 360**).
5. Tap **Start Service**. The physical controller inputs are grabbed exclusively and translated into your virtual Xbox 360 gamepad.
6. Launch your game and enjoy native controller gameplay.

## TODO / Planned Features

- [ ] **Manual In-App Button & Axis Remapping**: Add a dedicated manual mapping UI (similar to emulator button config screens) for controllers with non-standard physical layouts (e.g., Nintendo A/B/X/Y swap vs Xbox layout) or non-standard stick axis placements (such as Z/RZ axis sticks).
- [ ] **Bluetooth Device Filtering**: Filter the paired Bluetooth devices list to show only gamepad/controller class devices rather than all paired Bluetooth devices.
- [ ] **Gyro Aiming**: Route raw gyroscope data to DualShock 4 / DualSense motion controls for supported games.

## Credits

- **[Shizuku](https://github.com/RikkaApps/Shizuku)** by RikkaApps — Provides rootless access to `/dev/uinput` via the `shell` UID.
- **[SonicDX12/SteamController-Android](https://github.com/SonicDX12/SteamController-Android)** — The original repository from which this project was forked. Credit to SonicDX12 and contributors for developing the foundational Steam Controller driver, uinput service pipeline, and application architecture.
- **Development & Testing**: Developed by AI Assistant (Core implementation & JNI/Evdev architecture) and Lead QA / Testing by the repository maintainer.
- **SteamlessController** by ddeverill & Linux `hid-steam` driver — Reference material for Valve Steam Controller HID report parsing.

## License

MIT — See [LICENSE](LICENSE).
