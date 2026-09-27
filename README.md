# <img src="packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle

_"Deeper than a Peak. Wireshark is stuck in shallow waters."_

Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux, with a Dear ImGui GUI in a
deep-sea dark theme, licensed under GPL-2.0.
Fork of [CANgaroo](https://github.com/Schildkroet/CANgaroo).

[![Kraken Explorer in the dark theme: trace, live graph and CAN status](docs/view.png)](docs/demo.mp4)

▶ [Demo video (40 s)](docs/demo.mp4): live trace, time series, instrument panel, gauges, cursors
and statistics on five simulated buses.

```bash
cmake -S . -B build -G Ninja && cmake --build build --target kraken-explorer
# binary: build/src/kraken-explorer

# optional: app menu entry for the current user
sed "s|^Exec=.*|Exec=$PWD/build/src/kraken-explorer %f|" kraken-explorer.desktop > ~/.local/share/applications/kraken-explorer.desktop
install -D packaging/kraken-explorer.png ~/.local/share/icons/hicolor/256x256/apps/kraken-explorer.png
```

Interfaces, features, dependencies and permissions: see [docs/manual.md](docs/manual.md).
