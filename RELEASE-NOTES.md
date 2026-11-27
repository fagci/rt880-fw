# Unreleased

- **BAND multi-band scan:** long `2` now walks every AM broadcast band of the band plan (LW/MW and the 120–11 m SW bands) with each band's own step, instead of the fixed 14-band SW list. The overview is drawn incrementally: the full frame only on entry/scroll, a new find paints one column, and the frequency marker moves as a 5-px triangle, so a scan no longer repaints the whole area. `SIDE1/SIDE2` cycle the same plan bands; FM 64–108 MHz stays on long `1`.
- **Fix: freeze/fault on scanner range edit.** The app stack (1.8 KB) overlapped the LVGL heap and corrupted TLSF; entering the range editor could hang or hard-fault. The stack is now 5.9 KB (`_estack = 0x2000B000`, stage-2 magic at `0x2000BFF0` stays above it); scanner ranges are also normalised when `start >= end`. The fault handler now prints `pc`/`lr`/`xpsr` to UART.
- **Subtones end to end:** the scanner decodes CTCSS/DCS on a HOLD carrier (non-blocking, ~650 ms), stores the code in the find, shows it in the loot detail, carries it into channels and filters a channel scan by tone after the squelch opens. The channel format is v3 (18 bytes, `tone` + `toneSql`); v1/v2 records are read and rewritten on the next list change.
- **Nuisance and priority:** short `0` in the live scan skips a frequency for the rest of the session; `Scan → Auto blacklist` (Off..10 hits) blacklists a repeated find unless it is whitelisted; long `3` in the channel list sets/clears the priority channel, checked every 8 channels (`Radio → Priority`).
- **PC protocol:** the UART console gained `ch`, `ch add`, `ch del`, `loot`, `loot wipe` and `freq` for scripted channel/loot export/import and VFO tuning.
- **Settings N/A cleanup:** `Beep`, `Keylock`, `Active VFO`, `Battery save`, `Scan mode` (`CO`/`TO`/`SE`), `Scanlist`, `Rep STE`, `BL SQL mode`, `Always RSSI`, `Bat type`, `BAT cal` and the four Equalizer items are now functional; the former `Tone local` slot is reused as `Priority`. Key beep drives PA5 (2.7 kHz/40 ms, with a 20 ms AF-path lead-in because the LN4898 amp is gated by AF MUTE), battery save cycles `BK4819_RxSuspend` in the VFO after 5 s of silence (`Off`/`1:1`/`1:2`/`1:3`), the scanlist item reads/writes the real channel mask, `BAT cal` scales `battery_voltage_cv()`, `Bat type` selects a k1-fw discharge curve for the percent style, and the Equalizer ports the k1-fw AF-response tables (`REG_54/55/75` RX, `REG_44/45/74` TX; TX 3 kHz at 0 dB keeps the factory calibration).
- **Level in VFO:** with Display → `Level in VFO` enabled (default) the active VFO row now shows the same RSSI history graph as the scanner (240 columns ≈ 60 s, one direct-DMA column every 250 ms) in place of the LED S-meter; turning the item off brings the S-meter bar back.
- **Unified scan screens:** the scanner and FC now share one grid: the current frequency is large (`F_XL`, three decimals), the step is in the top line (`F_M`), and the dwell time is in the middle of the detail line in both. FC rounds its large frequency to the VFO step (the exact find stays in the loot row). FC also gets the scanner's embedded findings list: `UP/DOWN` enter/leave it, `1/2/3/4` sort, `SIDE1/SIDE2` black/white lists, `0` delete, long `SIDE2` save to a channel, long `5/8/9` channel operations, `MENU` capture to VFO.
- **Scanner screen:** there is no spectrum or waterfall (short multi-scan ranges never fill them). The live loot list uses that space (newest 10 finds with marks, age and peak dBm) plus an RSSI history graph modelled on `Level in VFO`/`SP_RenderGraph` in k1-fw. It samples every 250 ms during HOLD (240 columns ≈ 60 s) and draws only the new column, so listening is not interrupted by full-screen redraws.
- **Display gamma:** the ST7565 `Contrast` item is replaced by `Gamma` (ST7789 `GAMSET`): `G2.2` (default), `G1.8`, `G2.5` or `G1.0`.
- **Scan timings:** `Listen t/o` (default `None`), `Stay time` (2 s) and `Scan delay` (1800 µs, also `Speed` in the scanner menu) now work in the frequency and channel scanners; defaults follow k1-fw. `Open t`/`Close t` drive the BK4819 squelch delays.
- **PTT transmit (BK4819 A, FM)** with a global `TX enable` switch (default **Off**), `TX power` Low/Mid/High, 60 s TOT by default, busy-channel lock and mic gain. The RF sequence follows the related stock/uv98-rust plan (external TX gates, `REG_33` V/U TX bits, calibration-derived filter/deviation/power, PA code last). The factory calibration block is read from W25Q32 offset 0 (backup 0x1000); without the `0x9A` marker TX is refused. PF0/K200 follows the stock TX route and is restored after release; BK B stays asleep.

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
