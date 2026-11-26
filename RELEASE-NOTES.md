# v0.1.0

The first release of the C++/LVGL firmware for the Radtel RT-880 (AT32F423, BK4819 ×2, SI4732).

## Highlights
- **150 MHz clock** (the core used to run silently at 8 MHz): the screen and scan are an order of magnitude faster. The BK4819 (SPI) and SI4732 (I2C) buses are tuned for the new speed.
- **SI4732 on hardware I2C1, 400 kHz** (as in stock) with a patch ID check and fallback to 100 kHz / bit-bang: the SSB patch upload takes ≈0.8 s instead of 1.8 s.
- **VFO:** automatic modulation and step by band (band plan), step wrapping inside a band, band selection with `SIDE1/SIDE2` by group (broadcast / SSB / services), WFM on the SI4732.
- **Scanner:** step and dwell, arbitrary bounds, waterfall, noise calibration and threshold anti-sticking, loot list with black and white lists.
- **BC SW (long `6` in VFO):** broadcast SW scan on 5 kHz channels on the SI4732, automatic noise calibration, HOLD with listening, manual channel and band switching, spectrum/waterfall.
- **Analyser:** arbitrary start/end, "still" mode (the cursor marker listens), fixed cursor and waterfall artefacts.
- **FC (BK frequency counter):** board band filters, finds to loot, list skipping, squelch and sound as in the other modes.
- **C++ initialisation:** `Reset_Handler` runs `.init_array` (`RunInitArray`). Previously static constructors were not called: the scanner started in `PAUSE` with the range `0..0`, and the BC scanner/analyser defaults stayed zero.
- **Channels (long `7` in VFO):** up to 200 saved channels, 8 scanlists and ALL, BK scan by RSSI and SI by SNR, blacklist, saving from loot (long `5`/`8`), adding the VFO frequency to channels (long `5` in the list) and manual loading of the blacklist (long `9`); channels and the current scanlist survive power-off.
- **Sound/LED:** the green LED = squelch open; the hiss at start and in the scanner is gone; BK B sleeps (there is no reception on it per the schematic).
- Light/dark theme on the VFO screen (long `SIDE2`).

## Known issues
- Channels and the scanlist are stored in the tail of the external NOR flash; the stock channels at its beginning are neither read nor modified.
- The channel blacklist is not merged into loot after a reboot by itself — only with long `9` in the loot list.
- BK B is not used as a receiver (its input is not connected to the antenna).
- The key layout is in the README.
