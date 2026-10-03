# Windows port plan

Goal: a Windows x64 build of Kraken Explorer that colleagues can unzip and run: open and convert
logs, graph, DBC editor, Replay, Python scripts, and live CAN through the adapters they own.

Status (2026-10-03): plan only. Linux is the only supported platform today.

## What is already portable

Dear ImGui / ImPlot, GLFW + OpenGL 3, pugixml, nanosvg, doctest, zlib, pybind11 and libusb all
build on Windows. Most of `src/` is plain C++20/23 with `std::filesystem`, `std::jthread` and
`std::atomic`. A survey of Linux-only code found 8 files:

| File | Linux API | Windows replacement |
|---|---|---|
| `src/ui/frame_cache.cpp` | `mmap`, `memfd_create`, `fallocate`, `sendfile`, `madvise`, `mincore`, `readahead`/`pread` | `CreateFileMapping` / `MapViewOfFile` (pagefile-backed section for the in-RAM build), `SetFileInformationByHandle` (allocation), `WriteFile` from the view, `PrefetchVirtualMemory`, `ReadFile` |
| `src/core/rest_api.cpp` | BSD sockets, `poll` | Winsock 2, `WSAPoll` |
| `src/drivers/canblast.cpp` | UDP sockets, `poll` | Winsock 2 |
| `src/core/serial.cpp` | `termios`, `poll` | `CreateFile("\\\\.\\COMn")`, `SetCommState`, overlapped I/O (SLCAN, GrIP) |
| `src/drivers/kvaser.cpp` | `dlopen("libcanlib.so.1")` | `LoadLibrary("canlib32.dll")`: same CANlib API |
| `src/drivers/socketcan.cpp` | SocketCAN, libnl, `ip` via pkexec | not built on Windows |
| `src/drivers/usb_vendor/usb_vendor_libusb.cpp` | libusb | libusb with the WinUSB driver |
| `src/ui/setup_dialog.cpp` | `unistd.h` | small fix |

Smaller spots: `$HOME` / `$XDG_CONFIG_HOME` / `$XDG_CACHE_HOME` (settings, file dialog, cache),
`/proc/self/exe` (script window), `popen("gdbus ...")` for the dark-theme portal (theme.cpp), the
SocketCAN link / vcan buttons (can_status), pkexec handling (driver.h).

## Steps

1. **Toolchain and CI**
   * MSVC 2022 (17.8+ for `std::expected` / `std::move_only_function`) with vcpkg manifest for zlib,
     libusb, pybind11 and Python 3.12; CMake presets `windows-msvc`.
   * Keep FetchContent deps as they are; gate `pkg_check_modules(LIBNL ...)` and SocketCAN on `UNIX`.
   * GitHub Actions job on `windows-latest`: build, ctest (vcan tests skip themselves), upload a zip.
2. **Platform layer** `src/core/platform.h` (+ `platform_posix.cpp`, `platform_win32.cpp`)
   * Free functions only, as the rest of the code: `map_file`, `unmap`, `ram_file` (memfd /
     pagefile section), `allocate`, `copy_file_to`, `prefetch`, `available_ram`.
   * Paths: settings in `%APPDATA%\kraken-explorer`, cache in `%LOCALAPPDATA%\kraken-explorer\cache`,
     executable path via `GetModuleFileNameW`, UTF-8 everywhere (`activeCodePage` UTF-8 in the
     application manifest).
   * Dark theme: registry `AppsUseLightTheme` instead of the portal.
3. **Network and serial**: a Winsock shim (`WSAStartup`, `closesocket`, `WSAPoll`) for the REST API
   and CANblaster; a Win32 backend for `core/serial` (COM ports listed via SetupAPI).
4. **Drivers on Windows**
   * Kvaser: `canlib32.dll` from the Kvaser driver install, same function table.
   * PEAK: new `pcan.cpp` driver against PCAN-Basic (`PCANBasic.dll`), CAN and CAN FD, same size as
     the Kvaser driver.
   * Vector XL (`vxlapi64.dll`): later, if colleagues use Vector hardware.
   * gs_usb / candleLight and the STM32 CAN/LIN/AIO firmware: libusb + WinUSB. Add Microsoft OS 2.0
     descriptors to the firmware so Windows binds WinUSB without Zadig.
   * Hide SocketCAN link controls and "New vcan".
5. **Python**: ship the official embeddable Python (python312.dll + stdlib zip) next to the exe.
6. **Packaging**: portable zip first (exe, DLLs, Python, fonts are embedded already), then an
   installer (Inno Setup or MSIX). Without a code-signing certificate SmartScreen warns on first run.
7. **Testing**: ctest in CI; GUI smoke test on the Windows machine; MSVC AddressSanitizer build.

## Phases

1. Offline analyzer: logs (frame cache), conversion, graph, DBC/DBF/SYM, Replay, Python.
2. Live CAN: Kvaser, PEAK, SLCAN, gs_usb.
3. Installer and signing.

## Preparing the Windows machine (for building over SSH)

1. Settings > System > Optional features > add **OpenSSH Server**; start the `sshd` service and set
   it to Automatic. Allow port 22 in the firewall (the feature adds the rule).
2. Install **Visual Studio 2022 Build Tools** with "Desktop development with C++" (MSVC, Windows SDK,
   CMake, Ninja).
3. Install **Git for Windows** and **Python 3.12** (for the format-compat checks).
4. Add the Linux machine's `~/.ssh/id_ed25519.pub` to `C:\Users\<user>\.ssh\authorized_keys`
   (for an administrator account: `C:\ProgramData\ssh\administrators_authorized_keys`).
5. Tell Claude the machine's IP and user name; builds then run there like on the Linux build box.

## Open questions

* Which CAN adapters do the colleagues use (Kvaser, PEAK, Vector, other)? Sets the order in phase 2.
* Is a code-signing certificate available in the company?
