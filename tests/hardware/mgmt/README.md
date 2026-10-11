# Management command fixture

This image runs the real application command handler, USB, RF and NVS. Before
`main`, it saves the complete settings partition in RAM. Recovery restores that
snapshot and MCUboot rolls the unconfirmed fixture back to the previous image.
The runner checks the previous image hash, settings and identity after rollback.

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf tests/hardware/mgmt -d build/mgmt-hardware
python3 tests/hardware/mgmt/run.py --serial <board-serial>
```

The fixture injects one failed power-record write and a failed confirmation before
a successful retry. Its confirmation wrapper deliberately never confirms the
image; the application sees a confirmed boot to allow the attended command test.
Never confirm or ship this image. Keep the board powered until the runner restores
its original partition and firmware.

The runner checks every management command, including invalid values and lengths,
log reads and debug reports. It checks that erased pairing stays absent after the
application loop resumes, preserves the key and ID, and that factory reset clears
the running credentials. It repeats identity generation to exercise the real
entropy source. Restoration is reported separately; a failed assertion still
fails the run even if restoration succeeds.
