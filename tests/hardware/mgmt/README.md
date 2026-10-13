# Management acceptance fixture

Management acceptance must exercise configuration, status, logs, diagnostics and
recovery over the real USB transport. Before destructive scenarios it must
snapshot the complete settings partition and verify restoration of settings,
identity, pairing and the original image hash.

~~~sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf tests/hardware/mgmt -d build/mgmt-hardware
python3 tests/hardware/mgmt/run.py --serial <board-serial>
~~~

Required scenarios include invalid values and lengths, a failed settings write,
failed image confirmation followed by retry, pairing deletion after the application
resumes, and factory reset invalidating running credentials. Identity generation
must exercise the entropy source.

The fixture must remain unconfirmed so recovery can restore the partition and
roll back to the original firmware. Do not confirm or ship it. Keep power applied
until restoration is verified. Successful restoration must not hide a failed
acceptance assertion.
