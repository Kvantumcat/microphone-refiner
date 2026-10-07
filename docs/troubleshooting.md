# Troubleshooting

**`LNK1104: cannot open file 'microphone-refiner.exe'`**: the app is still
running. Quit it with `q`, then rebuild.

**`Capture and render devices have different sample rates`**: set all devices
to 48000 Hz, see [Prerequisites](../README.md#prerequisites).

**`Capture mix format is not IEEE float`**: pick a different Default Format for
the device in Windows Sound settings.

**Other apps don't hear anything**: the app must be running, and the other app
must use `CABLE Output` as its microphone. Some apps need a restart to see it.

**"Underwater" or warbling sound**: lower `a` toward 1.5-2.0 and raise `b` to
0.05-0.1.

**Noise comes back in fullscreen games**: under load the USB interference
spectrum can change. Learn a separate sample while the game runs (`l`, then
`sample-save gaming`) and switch with `sample-load gaming`.
