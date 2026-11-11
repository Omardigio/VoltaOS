# Known issues and limitations

Volta OS is in **beta**. This is the honest list of what does not work yet, or only partly. If you find
something that is not here, please [open an issue](https://github.com/Omardigio/VoltaOS/issues).

## Not working / not included yet

| Area | What is missing |
|---|---|
| **Navigation voice** | There is no speech engine: directions and speed camera alerts are on screen only. The *Voice guidance volume* slider is a placeholder. |
| **Instrument cluster** | Gear and speed always show "P" and "0 km/h": it is not connected to any vehicle data (CAN bus). The lock and airbag indicators are static too. |
| **Car buttons** | Lock, trunk and hood on the 3D car are visual only: they do not control a real car. |
| **Ambient lights** | The page and the color picker exist, but the service that talks to the LED strip is not included, so nothing lights up. |
| **Camera app** | The camera tiles are placeholders: no camera stream is connected. |
| **Calendar** | It is not connected to any online calendar (Google, Nextcloud…); events can only be added from code. |
| **Phone: destination from the phone** | Disabled in this version (it needs an external helper app). |
| **Spotify login with QR** | Not available: the login opens inside the UI. |

## Works only partly or with conditions

- **Fuel prices** work in **Italy only** (Italian Ministry open data).
- **Place search** needs a TomTom key; routes and traffic need a Mapbox token; the map style needs a MapTiler key
  (all free, see [API keys](api-keys.md)). Without them those features are off.
- **Spotify** playback needs `spotifyd` and, in general, a **Premium** account. The *Explore* categories are a fixed list.
- **YT Music** depends on `yt-dlp`: when YouTube changes something it can stop playing until you update it
  (`pip install -U --user --break-system-packages yt-dlp`).
- **Streaming sites in Theater** (Netflix, Prime Video, Disney+…) run in the embedded browser; video protected by DRM
  may not play depending on your system.
- **Phone over Bluetooth**: contacts, call history and SMS need the phone to allow sharing. Some phones (iPhones in
  particular) block the call history, and SMS sent over Bluetooth can fail silently on the phone side.
  Not every phone sends title and artist for the music it plays.
- **CarPlay / Android Auto** needs a Carlinkit dongle and was tested with very few phones.
- **Arcade** (Windows games) needs Bottles and `gamescope`, and depends on Wine: some games start, some do not, and
  there can be a delay before the first game starts.
- **Google login** in the browser works through a tablet identity; it can stop working if Google changes its checks.
- **Without the required system programs or Python libraries** the UI starts but is not 100% functional: the
  guided setup has a *System check* page that tells you what to install.

## Platform

- **Linux x86_64 with a Wayland session only** (tested on Debian 13 with GNOME). Not tested on other distributions,
  on X11-only sessions or on ARM.
- The AppImage needs `libfuse2t64` (or `--appimage-extract-and-run`).
- The bundled maps plugin is built for Qt 6.11.1; with another Qt (building from source) you must build it yourself.
- A GPU with working OpenGL drivers is required; on slow PCs turn on *Low performance mode*.
- The interface was developed and tested on a **single computer and one screen size**: other screens, touch
  controllers or scaling settings may show layout problems.

## Recently changed and not fully tested

- The app panel handle (dragging it down to close an app) was changed to close more easily.
- Several features were removed (3D map, place reviews, headlights button, an Easter-egg mode): if you see a
  leftover button or empty space where they were, please report it.
- The whole documentation and interface text were translated to English; some labels or translations
  (Spanish, French, Portuguese) can still be missing or wrong.

- **Android Auto without a dongle** is new: the USB cable mode has been tried with one phone only, and
  the Wi-Fi mode does not connect by itself yet (see [the guide](app/carplay-androidauto.md#android-auto-without-a-dongle)).

## Help me improve it

The most useful thing you can do is tell me **what does not work and what you would like to see**: open an
[issue](https://github.com/Omardigio/VoltaOS/issues) with what you did, what you expected and what happened.
