# OpenPhoneCam

A free, open-source tool that turns your Android phone into a low-latency camera source for OBS Studio, over a USB cable.

**Status: early prototype.** It works end to end on the setup listed under "Tested on", but many features are still missing (see the roadmap).

## How it works

    Phone camera -> hardware H.264 encoder -> TCP server on port 5000
        -> adb forward over USB -> OBS plugin (FFmpeg decoder) -> "OpenPhoneCam" source in OBS

- `android-app/` is the phone app (Kotlin). It captures the camera, encodes it with the phone's hardware encoder, and serves the stream on port 5000.
- `obs-plugin/` is the OBS Studio plugin (C). It connects to the stream, decodes it, and shows it as a video source.

## Features

- Free at every resolution the phone supports (720p, 1080p, 1440p, 4K, 8K are shown only if the phone and its encoder support them)
- Low-latency encoder settings (no B-frames, constant bitrate, real-time priority)
- USB connection through adb
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
4. On your computer run:

        adb forward tcp:5000 tcp:5000

   With Android Studio on macOS, adb is at `~/Library/Android/sdk/platform-tools/adb`.
5. Install the OBS plugin (see below), open OBS, and add a source with **Sources > + > OpenPhoneCam**.

The plugin reconnects by itself, so you can start the app, run the adb command, or open OBS in any order.

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
- [ ] Settings inside OBS (port, resolution, latency mode)
- [ ] Phone-side video stabilization
- [ ] Windows build of the plugin
- [ ] Wi-Fi connection
- [ ] HEVC option for 4K and above
- [ ] iPhone app
- [ ] Prebuilt downloads

## Known limitations

- The OBS plugin currently builds only for macOS.
- The phone app currently has to be built from source.
- Only H.264 is decoded by the plugin.
- Latency has not been measured accurately yet.

## License

GPL v3. See the LICENSE file. The OBS plugin is based on the official obs-plugintemplate.
