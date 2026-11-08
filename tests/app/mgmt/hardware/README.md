# Management hardware acceptance

This fixture exercises configuration, status, logs, diagnostics and recovery over
real USB. Its contract is specified in the
[management protocol](../../../doc/07-protocol.md#usb-management-mcumgr-smp) and
[firmware operation](../../../doc/06-firmware.md).

Run from the repository root with an LF board:

```sh
source tools/zephyr-env.sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
  tests/app/mgmt/hardware -d build/mgmt-hardware
python3 tests/app/mgmt/hardware/run.py --serial <board-serial> \
  --image build/mgmt-hardware/hardware/zephyr/zephyr.signed.bin
```

Before modifying persistent state, capture the complete settings partition and
original image hash. Required scenarios include invalid values/lengths, failed
settings writes, confirmation failure and retry, pairing deletion after resume,
and factory reset invalidating running credentials. New identity generation must
use hardware entropy. Status must distinguish saved configuration, live link
state and valid/unavailable sensor observations.

The fixture remains unconfirmed so recovery can restore the settings partition
and MCUboot can roll back. Do not confirm or ship it. Keep power applied until
restoration of identity, root key, pairing, settings and original image hash is
verified. A restored device cannot turn a failed assertion into a passing result.
See [acceptance requirements](../../../doc/10-debug.md#hardware-acceptance).

The runner requires native SMP on the starting firmware. It has no legacy transport
fallback. A one-time upgrade from an old image uses its existing updater to enter
MCUboot recovery. The fixture snapshots storage before application initialization;
new Settings intentionally clears unsupported layouts instead of importing them.
