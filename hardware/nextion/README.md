# Nextion display

Model: **NX3224F028_011**, Discovery series, 240x320 portrait.

Open `nextionScreen.HMI` in Nextion Editor, compile, export the `.tft`, and load it using the FAT32 microSD procedure. The `.HMI.sha256` file identifies the checked-in source. The compiled `.tft` is generated locally.

## Denied page

`page7.t1` uses font 5 (Arial16), a 222x74-pixel box and `txt_maxl=100`. Keep the object names and font IDs unchanged. Firmware inserts line breaks, aligns text at the top left, and shows long errors on numbered pages for six seconds each. The page timer stays disabled.

This update does not change the HMI, fonts or screen events. Keep credentials, captured faces and private preview URLs out of the repository.
