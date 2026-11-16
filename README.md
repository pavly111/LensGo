# OpenPhoneCam

A free, open-source tool that turns your Android phone into a low-latency camera source for OBS Studio, over a USB cable.

**Status: early prototype.** It works end to end on the setup listed under "Tested on", but many features are still missing (see the roadmap).

## How it works

    Phone camera -> hardware H.264 encoder -> TCP server on port 5000
        -> adb forward (set up automatically by the plugin) -> OBS plugin (FFmpeg decoder) -> "OpenPhoneCam" source in OBS

- `android-app/` is the phone app (Kotlin). It captures the camera, encodes it with the phone's hardware encoder, and serves the stream on port 5000.
- `obs-plugin/` is the OBS Studio plugin (C). It sets up `adb forward` for you (or connects over Wi-Fi), decodes the stream, and shows it as a video source.

## Features

- All streamable resolutions the phone reports (720p, 1080p, 1440p, 2K, 4K, 8K where supported; non-standard sizes are listed by their dimensions)
- H.264 and HEVC decoding, auto-detected from the stream
- Low-latency encoder settings (no B-frames, constant bitrate, real-time priority)
- Automatic `adb forward` over USB, no terminal command needed
- Wi-Fi streaming by entering the phone's IP address
- Frame rate selection (only the rates the phone's camera supports are shown)
- Open source (GPL)

## Requirements

- An Android phone with Android 8.0 or newer
- A USB data cable (some cables only charge)
- USB debugging turned on in the phone's Developer options
- OBS Studio 31 or newer (tested with 32.0.4) on macOS
- Android platform-tools (adb), installed with Android Studio

## How to use

1. Build and install the phone app (see "Building from source").
2. Connect the phone to your computer with the USB cable and allow USB debugging.
3. Open the OpenPhoneCam app, choose a resolution, and tap **Start streaming**.
4. In OBS, add a source with **Sources > + > OpenPhoneCam**. Leave **Connection** on **USB** and the plugin runs `adb forward tcp:5000 tcp:5000` for you.

   To stream over Wi-Fi instead, make sure the phone and computer are on the same network, set **Connection** to **Wi-Fi**, and enter the phone's IP address shown in the app (for example `192.168.1.20`).

   With Android Studio on macOS, adb is usually at `~/Library/Android/sdk/platform-tools/adb`. The plugin finds it automatically; you can set **adb path** in the source if needed.

The plugin reconnects by itself, so you can start the app, open OBS, or connect the phone in any order.

## Reducing latency

The stream is tuned for low latency out of the box: CBR, no B-frames, realtime encoder priority, a 1-second keyframe interval (plus a keyframe requested on connect), and small TCP buffers with Nagle disabled on both ends.

For the lowest delay in OBS:

- Set the source's **Latency mode** to **Lowest latency** (the default).
- Use **720p** if you don't need 1080p; encoding and decoding smaller frames takes less time.
- Select **60 fps** if the phone supports it (the button only appears when the camera can do it); otherwise 30 fps is used.
- Keep the phone on the **5 GHz** Wi-Fi band and close to the router; Wi-Fi power-save and weak signal add jitter.
- Keep the phone screen on (the app already does this) and avoid letting it sleep.

USB is still slightly more consistent than Wi-Fi because it has no wireless retransmits. Wi-Fi is convenient but subject to network conditions.

## Building from source

### Android app

1. Install Android Studio.
2. Open the `android-app` folder.
3. Connect your phone and click **Run**.

### OBS plugin (macOS)

You need the full Xcode app, CMake (`brew install cmake`) and OBS Studio.

    cd obs-plugin
    cmake --preset macos
    cmake --build --preset macos
    mkdir -p ~/Library/Application\ Support/obs-studio/plugins
    cp -R build_macos/RelWithDebInfo/openphonecam.plugin ~/Library/Application\ Support/obs-studio/plugins/
    install_name_tool -add_rpath /Applications/OBS.app/Contents/Frameworks ~/Library/Application\ Support/obs-studio/plugins/openphonecam.plugin/Contents/MacOS/openphonecam
    codesign --force --deep --sign - ~/Library/Application\ Support/obs-studio/plugins/openphonecam.plugin

Restart OBS, then check **Help > Log Files > View Current Log** for lines starting with `[openphonecam]`.

## Tested on

- Phone: Samsung Galaxy M51
- Computer: MacBook Air (Apple Silicon)
- OBS Studio 32.0.4

## Roadmap

- [x] Camera preview and resolution detection
- [x] Hardware H.264 encoding and USB streaming
- [x] OBS plugin for macOS
- [x] Settings inside OBS (port, resolution, latency mode)
- [x] Automatic adb port forwarding
- [x] Wi-Fi connection
- [x] HEVC option for 4K and above
- [ ] Phone-side video stabilization
- [ ] Windows build of the plugin
- [ ] iPhone app
- [ ] Prebuilt downloads

## Known limitations

- The OBS plugin currently builds only for macOS.
- The phone app currently has to be built from source.
- The plugin decodes H.264 and HEVC (auto-detected); other codecs are not supported.
- USB auto-forward relies on `adb` from Android platform-tools being installed and the phone being authorized for USB debugging.
- Latency has not been measured accurately yet.

## License

GPL v3. See the LICENSE file. The OBS plugin is based on the official obs-plugintemplate.
