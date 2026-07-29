# spex

**spex** is a realtime audio-input spectrograph built with [JUCE](https://juce.com/).
It listens to a live audio input device, renders a high-resolution spectrogram and
instantaneous spectrum, tracks a set of scalar spectral features over time, and
provides tools for measuring and matching spectral roll-off (dB/octave) against a
captured or imported reference.

The application is analysis-only: it never echoes audio to the output, so it is
safe to run alongside other audio software without feedback.

---

## Contents

- [Highlights](#highlights)
- [Building](#building)
- [Running](#running)
- [The display](#the-display)
- [Feature panel](#feature-panel)
- [Controls reference](#controls-reference)
- [Slope / roll-off analysis workflow](#slope--roll-off-analysis-workflow)
- [Capturing data to CSV](#capturing-data-to-csv)
- [Versioning](#versioning)
- [Project layout](#project-layout)

---

## Highlights

- **High-resolution analysis** — a 65,536-point (64k) FFT with a Hann window
  gives fine frequency resolution for steady-state material. Latency is
  irrelevant for this use case, so the window is deliberately large.
- **Log-frequency waterfall** — a scrolling spectrogram with the newest row at
  the bottom and a logarithmic frequency axis.
- **Instantaneous spectral envelope** — a live magnitude curve (dB vs.
  log-frequency) beneath the waterfall.
- **Six spectral features** tracked and plotted over time with autoscaling
  history sparklines.
- **Roll-off slope measurement** — linear and cubic-polynomial fits over a
  user-selectable frequency region, reported in dB/octave.
- **Reference matching** — freeze the current envelope, load a target audio
  file, or dial in a manual dB/octave target, then tune your source to match.
- **CSV capture/export** of the feature time series.

---

## Building

### Prerequisites

- CMake 3.22 or newer
- A C++20 compiler (MSVC 2022, Clang, or GCC)
- Git (used both to fetch JUCE and to embed the build commit hash)
- Platform audio/GUI development toolchain (e.g. the Windows SDK on Windows)

JUCE 8.0.2 is fetched automatically via CMake `FetchContent` — no manual JUCE
install is required.

### Configure and build

```powershell
cmake -B build -S .
cmake --build build --target spex
```

The resulting executable is written under
`build/spex_artefacts/<config>/spex.exe` (the exact path depends on your
generator and configuration).

> **Note:** on Windows the build must run in an environment where the MSVC
> toolchain and Windows SDK are on `PATH` (for example a *Developer PowerShell
> for VS*, or an IDE such as CLion / Visual Studio that sets this up for you).

The repository also contains preconfigured CMake build directories
(`cmake-build-debug`, `cmake-build-release`, etc.) for IDE use.

---

## Running

1. Launch `spex`.
2. Click **Audio Settings** to choose your input device, sample rate, and
   buffer size.
3. Feed audio into the selected input. The waterfall and spectrum update in
   realtime.

All input channels are summed to mono before analysis.

---

## The display

The central area is split into two vertically stacked views that share a
logarithmic frequency axis (from 30 Hz up to the current Nyquist frequency):

| Region | Content |
| --- | --- |
| **Top (waterfall)** | A scrolling spectrogram. Each horizontal line is one analysis frame; the newest frame appears at the bottom and older frames scroll upward. Brightness encodes magnitude. |
| **Bottom (envelope)** | The instantaneous spectral magnitude curve (dB on the Y axis). Overlays such as the slope fit, reference envelope, and peak markers are drawn here. |

Use **Pause** to freeze scrolling so you can inspect a moment in detail.

---

## Feature panel

The right-hand **Features** panel shows six scalar descriptors, each with a
live value and an autoscaling history sparkline:

| Feature | Meaning |
| --- | --- |
| **Windowed Peak** | Peak sample amplitude of the analysis window, in dBFS. |
| **Sliding RMS** | RMS level of the analysis window, in dBFS. |
| **Interpolated Spectral Peak** | Amplitude of the strongest spectral peak, parabolically interpolated for sub-bin accuracy, in dB. |
| **Spectral PAPR** | Peak-to-average power ratio of the spectrum, in dB. |
| **Local Spectral Crest** | Ratio of the peak bin to the local average — a measure of how "peaky" the spectrum is, in dB. |
| **Spectral Flatness** | Geometric-mean / arithmetic-mean ratio (0 = tonal, 1 = noise-like). |

The frequency band that feeds these features is set by the **Feature Range**
control, and the noise floor for the flatness computation is set by the
**Flatness Floor** control.

Toggle the panel with the **Features** button in the header.

---

## Controls reference

The header row holds the top-level buttons; the block beneath it (shown/hidden
with **Controls**) holds the analysis parameters.

### Header buttons

| Control | Action |
| --- | --- |
| **Audio Settings** | Opens the device selector (input device, sample rate, buffer size). |
| **Controls** | Shows/hides the parameter block. |
| **Features** | Shows/hides the feature panel. |
| **Pause** | Freezes/resumes the waterfall scroll. |
| **Start Capture** | Begins recording the feature time series (see [CSV capture](#capturing-data-to-csv)). |
| **Export CSV** | Writes the captured series to a `.csv` file (enabled once data is captured). |

### Parameter block

| Control | Type | Purpose |
| --- | --- | --- |
| **Feature Range** | dual slider | Frequency band (min/max Hz) used for feature analysis. |
| **Flatness Floor** | slider (−180…−30 dB) | Power floor applied when computing spectral flatness. |
| **Average** | toggle | Time-averages the displayed spectrum for a smoother envelope. |
| **Clear Avg** | button | Resets the accumulated average. |
| **Peak marks** | toggle | Overlays interpolated peak markers on the envelope. |
| **Pre-gain** | slider (−12…+12 dB) | Gain applied to the input before analysis. |
| **Freq Warp** | slider (1…4) | Warps the frequency axis to emphasize a region of the spectrum. |
| **Slope Region** | dual slider | Frequency region (min/max Hz) over which the roll-off slope is fit. |
| **Set Target** | button | Freezes the current live envelope as the reference. |
| **Clear Target** | button | Removes the reference envelope. |
| **Load Target Audio** | button | Loads an audio file and uses its spectrum as the reference. |
| **Manual dB/oct** | toggle + slider (−24…0 dB/oct) | Uses a manually dialed roll-off as the target instead of a captured one. |
| **Cubic fit** | toggle | Adds a cubic-polynomial fit to the roll-off region in addition to the linear fit. |
| **Fit peaks** | toggle | Fits the roll-off only to spectral peaks (harmonic tops) rather than the whole envelope — useful for harmonic sources such as a filtered sawtooth. |
| **Peak Floor** | slider (−80…0 dB) | Minimum height a peak must reach to be tracked by the peak-only fit. |

---

## Slope / roll-off analysis workflow

**spex** is designed to help match one source's spectral roll-off to another.

1. **Choose the region.** Drag the **Slope Region** dual slider to bracket the
   frequency band you care about (for example 3 kHz – 20 kHz). The measured
   slope in **dB/octave** is displayed in the readout above the display.
2. **Pick a fit style.**
   - Leave **Fit peaks** on for harmonic material so the regression follows the
     harmonic tops; raise **Peak Floor** to ignore noise or sub-fundamental
     ripple.
   - Enable **Cubic fit** to see curvature that a straight-line fit misses.
3. **Establish a target** using any one of:
   - **Set Target** — freeze the current live envelope.
   - **Load Target Audio** — analyze a reference file.
   - **Manual dB/oct** — dial in an ideal slope directly.
4. **Tune your source** until its live envelope and slope match the reference.
   Use **Clear Target** to start over.

Enabling **Average** first yields a steadier envelope and a more stable slope
estimate.

---

## Capturing data to CSV

1. Press **Start Capture** to begin logging every analysis frame's feature
   values along with a timestamp. The button toggles to stop capture.
2. Press **Export CSV** and choose a destination file.

The exported file contains one row per captured frame with columns:

```
time_seconds,
windowed_peak_dbfs,
rms_dbfs,
interp_spectral_peak_db,
spectral_papr_db,
local_spectral_crest_db,
spectral_flatness
```

---

## Versioning

The application version is defined in [`version.toml`](version.toml):

```toml
major = 1
minor = 0
patch = 0
```

At build time CMake parses this file and combines it with the current Git
commit hash to generate a `Version.h` header (into `<build>/generated/`). The
version string and short commit hash are shown subtly in the top-left of the
main window (for example `v1.0.0  bec9c3f`). A `-dirty` suffix on the hash
indicates uncommitted changes were present when the binary was built.

To release a new version, edit `version.toml` and rebuild — the header and the
on-screen label update automatically.

---

## Project layout

| Path | Description |
| --- | --- |
| `main.cpp` | JUCE application/window bootstrap. |
| `MainComponent.h` / `.cpp` | Top-level UI: controls, layout, capture/export, and painting. |
| `SpectralDisplayComponent.h` | Waterfall + envelope rendering, FFT, averaging, slope/reference fitting. |
| `SpectralFeatureAnalyzer.h` | Computes the six scalar spectral features from a magnitude spectrum. |
| `version.toml` | Human-edited version numbers. |
| `cmake/Version.h.in` | Template for the generated version header. |
| `cmake/GenerateVersion.cmake` | Parses `version.toml` and the Git hash, then emits `Version.h`. |
| `CMakeLists.txt` | Build configuration; fetches JUCE and wires up version generation. |
