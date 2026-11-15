# Raiden Fighters Remix Collection native port

Linux x86-64 and Android ARM64 port in development for PS4 CUSA53688 v1.20,
using the shared Tempest, PAC-MAN CE2, Akka Arrh and Polybius runtime.

The collection includes Raiden Fighters, Raiden Fighters 2 and Raiden Fighters
Jet, with Japanese and US variants. The supplied matching base and update have
been inspected; the executable and bundled libraries prepare and link. Linux renders the original title, game selection and arcade attract sequence
at 60 FPS in steady sections. Player-stage and Android device verification are
in progress. See the
[current status](docs/raiden-fighters-port-status.md) for verified behavior.

Android has a separate application ID, `com.crownparkcomputing.raidenfighters`.
L3+R3 opens the shared Settings menu. Graphics driver supports bundled Turnip,
the system driver and importing compatible ARM64 driver ZIPs, applied on relaunch.

Original game packages, extracted data, prepared images, drivers, signing keys
and saves remain local and excluded from Git. The APK contains no game data.

See [Linux build instructions](docs/building.md), [Android](android/README.md)
and [source origins](THIRD_PARTY_NOTICES.md). Run `bash build.sh`, prepare your
supported dump with `python3 run.py prepare --game game`, then use
`./play-raiden-fighters.sh`. Shared configuration names remain `TEMPEST_*`.
