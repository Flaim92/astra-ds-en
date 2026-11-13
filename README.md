<div align="center">

<img src="res/astra-256.png" alt="Astra DS" width="128">

# Astra DS

**A Discord client written in C that runs in about 3 MB of RAM.**

C11 · Windows, Linux and macOS · no Electron, no browser, no GPU

![C11](https://img.shields.io/badge/language-C11-00599C)
![Windows and Linux](https://img.shields.io/badge/Windows%20%7C%20Linux-ready-2ea44f)
![macOS](https://img.shields.io/badge/macOS-untested-f0b232)
![Memory](https://img.shields.io/badge/idle_memory-~2.9_MB-5865F2)
![Discord features](https://img.shields.io/badge/Discord_features-86%25-f0b232)
![License GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)

[![Telegram](https://img.shields.io/badge/Telegram-@astradsoff-26A5E4?logo=telegram&logoColor=white)](https://t.me/astradsoff)

**📣 [Telegram channel](https://t.me/astradsoff): news, releases and discussion (in Russian)**

![Astra DS](docs/screenshots/main.png)

*A Discord-style window drawn with colored text characters: polls, a voice message, bot buttons and a link card. Everything here is fake demo data.*

</div>

> [!IMPORTANT]
> **Astra DS comes in two builds.**
> - **Astra DS** (regular): messages, servers, friends, voice channels and calls. It has no video code at all, so it is the lightest.
> - **Astra DS Video**: everything above plus **screen sharing, watching other people's streams and camera** (with eye hiding). The file is called `astra-ds-video`.
>
> If you want streams or the camera, download **Video**. The regular build has no buttons for them.

> [!WARNING]
> **Third-party clients break Discord's Terms of Service**, and your account can be banned for using one.
> Use it at your own risk, ideally with a secondary account. This project has no affiliation with Discord Inc.
> To just look around, run `astra-ds --demo`: fake data and no network.

> [!NOTE]
> The interface is available in **English**: use the RU | EN switch on the start screen or run with `--lang=en`. The code and build are the same as the [Russian repository](https://github.com/Flaim92/astra-ds).

## Two builds

| | Astra DS | Astra DS Video |
|---|---|---|
| Messages, servers, friends, DMs | yes | yes |
| Voice channels and calls (DAVE encryption) | yes | yes |
| Screen sharing with sound | no | yes |
| Watching other people's streams | no | yes |
| Camera: yours and other people's | no | yes |
| Eye hiding on your camera | no | yes |
| Program file | ~13 MB | ~22 MB |

The two builds update separately: the regular one only offers regular updates, Video only offers Video updates.

## Download

Ready-made builds for Windows 10/11, Linux and macOS are in [Releases](../../releases/latest):

| System | Regular | Video |
|---|---|---|
| Windows 10/11, x64 | `astra-ds-<version>-windows-x64.zip` | `astra-ds-video-<version>-windows-x64.zip` |
| Linux x64, glibc 2.36+ | `astra-ds-<version>-linux-x64.tar.gz` | `astra-ds-video-<version>-linux-x64.tar.gz` |
| macOS 12+, Apple Silicon ⚠️ | `astra-ds-<version>-macos-arm64.tar.gz` | `astra-ds-video-<version>-macos-arm64.tar.gz` |
| macOS 12+, Intel ⚠️ | `astra-ds-<version>-macos-x64.tar.gz` | `astra-ds-video-<version>-macos-x64.tar.gz` |

- **Windows:** a single exe, no installer. The build is not code-signed, so SmartScreen will warn you: click "More info", then "Run anyway".
- **Linux:** you need `libasound2` and `libx11-6`. For the captcha window, install `libwebkit2gtk-4.1-0`. For Video: stream sound goes through PulseAudio or PipeWire, screen sharing on Wayland through `xdg-desktop-portal` and PipeWire. Regular distributions already have all of this.
- **macOS (⚠️ not tested on a real Mac):** the build passes the self-test on GitHub's macOS servers, but nobody has run it live yet. It is not signed by Apple: after unpacking, run `xattr -dr com.apple.quarantine astra-ds`, otherwise macOS will not let it start. Screen sharing is not available on Mac yet; watching streams and the camera are. Captcha through VLESS needs macOS 14+. If you have a Mac, please tell us in [Issues](../../issues) what works.

Checksums are in `SHA256SUMS.txt` next to the builds. You can also build it yourself, see below.

## If Discord locks your account

Discord disables an account or forces a password reset when it thinks the account is a bot or was compromised. It can happen with any client, but third-party clients trigger it more often. The usual causes are:

- joining or leaving servers;
- reconnecting often, which includes restarting the app many times in a row;
- opening DMs with people you don't know;
- changing your friends list, especially many friend requests in a row.

**Log in with email and password.** If two-factor authentication is on, enter the code from your authenticator app after the password. QR code login is unreliable right now: Discord may ask for a captcha and then refuse to accept it. It behaves the same way in a browser, so this is not an Astra DS bug.

It's better to do all of this in the official client. To join a server from an invite, Astra DS will send you to the browser anyway: Discord requires the official client for that. If Discord ended your session after a captcha, log in again and check the official app for a suspicious activity notice. If your account was locked, change your password on discord.com, and if that doesn't help, [contact support](https://support.discord.com/hc/en-us/requests/new?ticket_form_id=360000029731).

## If there is no connection to Discord

If login says "No connection to Discord" while the internet works:

- **Discord is blocked by your provider.** Check whether https://discord.com opens in a browser with VPN turned off. If it doesn't, set up your own VLESS server in the app (`--vless`).
- **A VPN app in "system proxy" mode** (v2rayN, Hiddify, Nekoray and similar). Astra DS does not use the system proxy. Turn on TUN mode ("VPN for the whole system") in the VPN app or set up a VLESS server in Astra DS.
- **An antivirus or firewall** blocks the program from the network. Allow it.

VLESS is off by default: without your link all traffic goes directly. You can turn it off in **Settings → Privacy → VLESS**.

## If your antivirus complains

An antivirus may mistake any Discord client for a token stealer: the code contains the same words, "token" and "Authorization". The token is only ever sent to Discord and is stored in the system keychain (Credential Manager on Windows, Secret Service on Linux, Keychain on macOS). The code is open, you can read it and build it yourself. Only download Astra DS from this repository.

## Why

The official Discord is basically a whole web browser inside an app. For comparison:

| | Idle memory | Idle CPU | Program file |
|---|---|---|---|
| Discord (Electron) | usually 300-600 MB¹ | noticeably above zero | hundreds of MB |
| [larperCord](https://github.com/RshaCuDeVidro/larperCord) (Zig, SDL3) | ~130 MB² | 0-0.4%² | ~14 MB |
| **Astra DS** | **~2.9 MB**³ | **0%** | **~13 MB** (Video ~22 MB) |

<sub>¹ An estimate from Task Manager, depends on the number of servers. We did not measure it side by side.
² From the larperCord README, not our measurement.
³ Process private memory, Windows 11: median of five runs, 140x40 cell window, 2 s wait. A channel with images is 2.9-3.1 MB, a voice call with end-to-end encryption 4-5 MB. Measured on the regular build.</sub>

How it gets there:

- **one thread and no polling loop.** The program sleeps until an event arrives, so it uses 0% CPU when idle;
- **nothing extra in memory.** Discord events the client doesn't need are dropped without being parsed. Login data is parsed one server at a time;
- **images are drawn on the fly** from small previews. Attachment previews are freed as soon as they leave the screen. Avatars are kept in memory up to 64 KB so scrolling doesn't download the same faces again. Nothing is written to disk;
- **voice and video only exist during a call.** Threads, codecs, the camera and encryption are created when you join and fully freed when you leave;
- **heavy things load on demand.** Server emoji, slash commands, forums and profiles arrive when you open them and are freed when you close them.

## Features

Out of an inventory of 162 Discord screens and features, **86%** are done (84% if partially done ones don't count).

- **Messages.** Sending, replies (with or without pinging the author), editing, deleting, reactions, silent messages, forwarding, per-channel drafts. An unsent message shows up grey, and if it fails you can retry it. History loads as you scroll up, all the way to the start of the channel.
- **Discord markdown.** Bold, italic, strikethrough, inline code, code blocks with a "Copy" button, headings, lists, quotes, spoilers. A `[text](url)` link shows the real address if it doesn't match the text.
- **Attachments.** Images and avatars as colored ASCII, full-screen viewer, text file previews, voice messages (streamed playback), sending files up to 10 MB with spoiler and description, drag and drop into the window.
- **Bots.** BOT tag, buttons and menus, slash commands with option input, link cards, polls with voting.
- **Servers.** Channels by category with permissions, server folders, threads and forums (create, join, archive, new post), pinned messages, slowmode with a countdown.
- **Management.** Create, edit, leave or delete a server. Channels and categories: create, rename, reorder, set permissions. Moderation for those who have the rights: timeout, kick, ban, roles, moving people in voice.
- **People.** Friends with statuses, requests, blocking, ignoring, private notes, full profile, group DMs. Message requests and spam are kept separate from DMs, and images there don't load until you reply.
- **Voice.** Voice channels, DM and group calls. DAVE end-to-end encryption is mandatory: without it your voice is not sent. Per-user volume and your own microphone volume (up to 400%), microphone and speaker selection, mic test, automatic threshold, noise suppression (SpeexDSP or RNNoise), push-to-talk on any key or side mouse button, voice changer. Voice settings are kept between launches.
- **Navigation.** Channel tabs, quick switcher Ctrl+K, search with `from:` `in:` `has:` `before:` `after:` filters, an inbox with mentions, Discord message links open inside the app.
- **Comfort.** Zoom with Ctrl+= and Ctrl+-, full screen with Alt+Enter, column widths by mouse, notifications and a tray icon, focus mode, alert words, word filter, English interface (`--lang=en` or the RU | EN switch on the login screen), auto-update with a signed manifest.
- **Three modes.** A window with mouse support, a full-screen terminal mode (`--tui`) and a line-by-line IRC-like mode (`--cli`).

### Only in Astra DS Video

- **Screen sharing.** The whole screen or one window, 1280x720 at 30 frames per second, in voice channels and calls. With sound: a single window is shared with its program's sound, the whole screen with all system sound except Astra DS itself (call voices do not go into the stream). On Linux it works on both X11 and Wayland (GNOME, KDE, sway and others, through the system source picker). On Wayland a single window is shared without sound for now: the system does not say which program it belongs to.
- **Watching other people's streams**: H.264, HEVC, VP8, VP9 and AV1.
- **Camera**: your own and other people's, as tiles or large (Alt+arrows, Alt+F). Turn it on with the `[]/` button, F12 or Alt+C; pick the camera and quality (auto, 720p, 480p, 360p) in the Camera dialog. Windows, Linux (V4L2) and macOS (AVFoundation).
- **Eye hiding on your camera.** The program finds your face in every frame and covers the eyes with a block of noise and random characters; the covered pixels cannot be recovered. If no face is found or the program is not sure, others see the Astra DS logo instead of the frame, and your preview says "Face not found". On by default, turn it off with Alt+E or the "Hide eyes" checkbox in the Camera dialog. Everything runs on your computer, about 2 ms per frame, and frames go nowhere except the call. Limitation: if a second person is in the frame and the program doesn't find their face (cut off by the frame edge or turned far away), their eyes may stay visible.

**Not included:** animated GIFs, plugins.

## Privacy and security

- **No logs, no telemetry, nothing on disk.** Only the token and voice settings (volume, devices, threshold) go into the system keychain, there are no separate files. Everything else lives in memory and is wiped when freed.
- **The token lives in locked memory** that is never swapped out and never ends up in crash dumps.
- **Your own VLESS (Reality) server** for all traffic, voice included. It is used only if you set a link yourself. If the tunnel goes down, the app does not silently fall back to a direct connection, it asks you first.
- **Streamer mode** hides your email, the VLESS server address, invite links, file paths and DM text.
- **Hide the window from screenshots** (Video, Windows and macOS): optionally, the window is not visible in Snipping Tool, screenshots or other people's screen sharing. Off by default.
- **Panic key:** pressing it three times wipes the token, closes the app and clears the screen.
- **Captcha** opens in a separate window of the app, and the action is retried automatically afterwards.
- **Notifications without details.** By default they only say "new message": Windows keeps notification history on disk.
- **Everything from the network is validated.** Control characters and text direction overrides are stripped, and data parsing is tested with fuzzers (`tools/sec/`).

## Screenshots

<table>
<tr>
<td><img src="docs/screenshots/chat.png" alt="Channel with bots"></td>
<td><img src="docs/screenshots/forum.png" alt="Forum"></td>
</tr>
<tr>
<td align="center">Bots, a spoiler and a forwarded message</td>
<td align="center">Forum</td>
</tr>
<tr>
<td><img src="docs/screenshots/search.png" alt="Search"></td>
<td><img src="docs/screenshots/voice.png" alt="Voice channel"></td>
</tr>
<tr>
<td align="center">Search with pages</td>
<td align="center">Voice channel</td>
</tr>
<tr>
<td><img src="docs/screenshots/friends.png" alt="Friends"></td>
<td><img src="docs/screenshots/settings.png" alt="Voice settings"></td>
</tr>
<tr>
<td align="center">Friends, groups and requests</td>
<td align="center">Voice settings</td>
</tr>
<tr>
<td colspan="2"><img src="docs/screenshots/start.png" alt="Start screen"></td>
</tr>
<tr>
<td colspan="2" align="center">Start screen: connecting through VLESS with a "Don't ask again" checkbox</td>
</tr>
</table>

*The screenshots show the Russian interface; switch to English with RU | EN or `--lang=en`.*

## Useful flags

```
--demo            demo with fake data, no network
--tui             full-screen terminal mode
--cli             line-by-line mode, like IRC
--lang=ru|en      interface language
--no-save         don't save the token, enter it on every launch
--logout          delete the saved token and exit
--no-images       don't load images and avatars
--vless           set or change your VLESS server
--no-vless        no VLESS for this launch, direct connection
--panic=F12       panic key (press three times)
--invisible       log in as invisible
--no-ack          don't mark messages as read
--ptt             push-to-talk instead of voice activation
--tray            minimize to tray (Windows)
--hq              higher voice quality at the cost of CPU
--mic=NAME        microphone (part of the device name)
--mic-gain=N      microphone volume, % (0..400)
--out=NAME        output device (part of the name)
--devices         list audio devices
--no-update       don't check for updates
--version         program version
--selftest        offline self-test
```

## Building

**Windows** (you need WinLibs GCC, CMake, Ninja, NASM, Python 3 and Git for Windows):

```
bash deps/build-deps.sh     # once: build dependencies from source
bash build.sh               # regular: build/astra-ds.exe
VIDEO=1 bash build.sh       # Video: build/astra-ds-video.exe
```

**Linux** (Debian or Ubuntu):

```
sudo apt install build-essential cmake ninja-build nasm python3 perl git curl xz-utils libasound2-dev libx11-dev pkg-config
bash deps/build-deps.sh
bash build.sh               # regular: build/astra-ds
VIDEO=1 bash build.sh       # Video: build/astra-ds-video
```

**macOS** 12+ (Xcode Command Line Tools and Homebrew):

```
brew install cmake ninja nasm gnu-sed coreutils llvm
bash deps/build-deps.sh
bash build.sh               # regular: build/astra-ds
VIDEO=1 bash build.sh       # Video: build/astra-ds-video
```

Dependencies (zlib, OpenSSL, curl, opus, libdave, SpeexDSP, RNNoise, libjpeg-turbo, and for Video also FFmpeg and dav1d) are built from source with checksum verification and linked statically. There is one exception: the prebuilt libcurl-impersonate library in `third_party/impcurl/`, used only for login requests. It is embedded in the program and is not a separate file next to it. System libraries for video on Linux (PulseAudio, PipeWire, D-Bus) are loaded only when needed and are not required to start the program.

To check that everything works: `build/astra-ds --selftest`. It runs more than 470 offline checks of the interface, data parsing, voice and security, more than 550 in the Video build.

## Code layout

| File | What's inside |
|---|---|
| `src/ui.c` | interface: Discord-like layout, mouse and keyboard |
| `src/gui_win.c`, `gui_x11.c`, `gui_mac.m`, `gui_grid.c` | character-grid window for Windows, Linux and macOS |
| `src/plat_win.c`, `plat_posix.c`, `compat_apple.h` | system layer: threads, network, token storage |
| `src/store.c` | data model, permissions, Discord events |
| `src/memlist.c` | server member list by roles |
| `src/net.c` | Discord requests and the persistent connection |
| `src/voice.c`, `voicefx.c` | voice, DAVE encryption, voice messages, voice changer |
| `src/img.c` | images as colored ASCII |
| `src/login.c`, `captcha.c` | login, QR code, captcha |
| `src/vless.c`, `xlite_c.c`, `tls13.c` | VLESS tunnel |
| `src/secret.c`, `panic.c` | protected memory and the panic key |
| `src/update.c` | auto-update with a signed manifest |
| `src/golive.c`, `voice_stream.c`, `streamtest.c` | Video: streams, sending and watching |
| `src/share_x11.c`, `share_pw.c`, `portal_wl.c` | Video: screen capture on Linux (X11, Wayland via portal and PipeWire) |
| `src/viddec.c` | Video: decoding other people's streams and cameras |
| `src/cam.c`, `cam_mac.m`, `voice_cam.c` | Video: camera (Media Foundation, V4L2, AVFoundation) and sending it into a call |
| `src/anon.c` | Video: eye hiding on your camera |
| `src/selftest.c` | self-test |
| `tools/sec/` | fuzzers and security test rigs |
| `tools/uitest/` | checking every screen with hidden screenshots |

## Support the project

A bit about me. I worked as a Java developer for three years and then lost my job. I tried to start my own business twice, both attempts failed, and I ended up in debt: right now it is about 240 thousand. At the same time I'm retraining for a new profession, Go developer.

That's why any help matters to me, even a very small one. If Astra DS is useful to you and you'd like to support me and the project, you can send a donation in USDT (TRON network, TRC-20):

```
TXxTPQ14xbxrr2D6HeG3d27K3fm8kaKWus
```

Send only USDT on TRC-20: other coins or networks sent to this address will be lost. Thank you!

## Author

Hi! I'm Flaim92, and I'm looking for a job as a **Go developer**. If you need someone who loves squeezing the most out of code and isn't afraid of low-level work, write to me: [flaim922@gmail.com](mailto:flaim922@gmail.com).

Questions, ideas and bug reports about Astra DS are welcome by email or in [Issues](../../issues).

## License

GPL-3.0, see [LICENSE](LICENSE). Third-party code keeps its own licenses, with full texts in `third_party/LICENSE`:

- in `third_party/`: cJSON (MIT), the WebView2 header (Microsoft's BSD-style license), libcurl-impersonate (MIT), the minih264 H.264 encoder (CC0), the pico face detector (MIT, Video only);
- built from source: zlib, OpenSSL (Apache-2.0), curl, Opus, libdave and MLSpp, nlohmann/json, Brotli, SpeexDSP and RNNoise (BSD-3), libjpeg-turbo (IJG and BSD-style), for Video also FFmpeg (LGPL-2.1, no GPL parts) and dav1d (BSD-2);
- the VLESS tunnel: Xray-core (MPL-2.0), Go and its modules.

Astra DS is not affiliated with Discord Inc. Discord is a trademark of Discord Inc.
