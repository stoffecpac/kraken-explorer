# <img src="packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle

Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux and Windows, with a Dear ImGui GUI.
Based on [CANgaroo](https://github.com/Schildkroet/CANgaroo), licensed under GPL-2.0.

![Trace view](docs/view.png)

```bash
cmake -S . -B build -G Ninja && cmake --build build   # binary: build/src/kraken-explorer
```

Interfaces, features, dependencies and permissions: see [docs/manual.md](docs/manual.md).
