# Patches carried on top of upstream modules

West pulls these modules from upstream and they are built as they come; anything zlrs needs on top
lives here as a patch, so a fresh `west update` plus the patches below is the whole story. The
alternative — editing the module in place — leaves the change in a directory nobody clones.

Apply from the workspace root (the directory that holds the module checkouts):

    git -C usp_zephyr apply "$PWD/zlrs/patches/usp_zephyr/0001-lr20xx-hal-bounded-busy-and-ready-check.patch"

| Patch | Why |
| --- | --- |
| `patches/usp_zephyr/0001-lr20xx-hal-bounded-busy-and-ready-check.patch` | The LR20xx HAL waits for the radio's BUSY line with a ten-minute timeout and then stops the CPU (`k_oops`). A single wedged SPI access therefore took the whole link down to the watchdog. The patch bounds the wait at 1 s, returns an error instead of stopping, and propagates it through the HAL read/write entry points so callers see a failed access and retry. |

Both halves are upstream-report-worthy; until they land there, this is how a clean checkout builds
the image the bench runs.
