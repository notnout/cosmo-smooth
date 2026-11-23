# Episode Data Directory

This repository does **not** include the copyrighted data files from
*Cosmo's Cosmic Adventure* (1992). To play, supply your own `.STN` and `.VOL`
files from the original shareware or registered release:

| Episode | Required files |
|---|---|
| Episode 1 (*Forbidden Planet* — shareware or registered) | `COSMO1.STN`, `COSMO1.VOL` |
| Episode 2 (*Forbidden Planet* — registered) | `COSMO2.STN`, `COSMO2.VOL` |
| Episode 3 (*Forbidden Planet* — registered) | `COSMO3.STN`, `COSMO3.VOL` |

## Option 1: Automatic ZIP packaging for DOSBox Pure / RetroArch

1. Drop either the extracted `.STN` / `.VOL` files **or** your original episode
   `.zip` archives into this `gamedata/` folder.
2. From the repository root, run:

   ```bash
   ./package.sh
   ```

   *(Or point it at any existing directory without copying files: `./package.sh /path/to/your/cosmo-files`.)*

3. Ready-to-play DOSBox Pure ZIPs will be written to `dist/`:
   - `dist/cosmo-smooth-ep1.zip`
   - `dist/cosmo-smooth-ep2.zip`
   - `dist/cosmo-smooth-ep3.zip`
   - `dist/cosmo-smooth-all-episodes.zip` (when 2 or more episodes are found)

## Option 2: Manual setup (any DOSBox or 32-bit DOS setup)

Copy `bin/COSMO1.EXE` (and/or `COSMO2.EXE`, `COSMO3.EXE`) into the same folder
as your `COSMOn.STN` and `COSMOn.VOL` files, replacing the original 16-bit
`COSMOn.EXE`, and launch it in DOSBox.
