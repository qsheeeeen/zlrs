# Zephyr dependency patches

`uart-pl011-nonblocking-tx.patch` targets the manifest's Zephyr v4.4.0.
`tools/setup-zephyr.sh` applies it after `west update` and accepts an already
applied patch without changing other workspace edits. For an existing workspace:

```sh
git -C ../zephyr apply "$PWD/patches/zephyr/uart-pl011-nonblocking-tx.patch"
```

The upstream PL011 driver repeatedly invokes the TX callback until the caller
disables TX interrupts. CTS can stop the hardware indefinitely, leaving this
loop occupying the caller's work queue. Prime the FIFO once on each enable and
let hardware interrupts handle subsequent space. Report FIFO space as TX-ready
so another short fragment can refill a FIFO below its interrupt watermark.

The DATA callback disables TX interrupts as soon as its queue empties. The
hardware fixture in `tests/hardware/serial` exercises blocked CTS, continued
work-queue service, automatic RTS pause/resume and short-fragment restarts.
