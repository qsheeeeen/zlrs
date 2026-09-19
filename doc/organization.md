# Repository organization

This document specifies how the repository is put together: what a piece of
hardware is made of, how software discovers it, which parts of the software
adapt to hardware and which parts build features on top, and which rules keep
the tree from drifting. It is the reference for "where does this file go" and
for adding hardware.

Scope decisions that everything below follows:

- **Zephyr only.** The firmware is a Zephyr application plus Zephyr drivers. No
  RTOS abstraction layer, no bare-metal variant.
- **One firmware image per build, both roles.** A TX and an RX run the same
  image; the role is runtime configuration with a compile-time default.
- **Hardware first.** The hardware model is explicit and the software adapts to
  it; features are subdivided on top of what the hardware provides, never
  assumed.
- **Capability, not identity.** Software never branches on a board name or a
  radio part number beyond the driver that owns that part.

## 1. Hardware model

```
hardware = MCU (A) × radio chip (B) × radio design (LF/HF, band and calibration) × ports
```

| Axis | What it fixes | Where it is declared |
| --- | --- | --- |
| MCU | core, memory, available Zephyr devices (SPI, UART, USB, NVS, watchdog) | board devicetree + SoC definitions |
| Radio chip | register interface, modulation set, driver | `compatible` on the radio node, driver under `drivers/` |
| Radio design | frequency coverage, matching network, power and calibration tables | properties on the `zlrs_rf` node of the board |
| Ports | how many logical ports exist and what each one is wired to | aliases and nodes in the board devicetree |

A **port is a logical port**: one UART endpoint plus one USB CDC endpoint, used
as a pair (the application picks whichever endpoint the host is using). Control
data and data never share one port: a port carries one link.

### 1.1 Port forms

| Form | Aliases (UART / CDC) | Link | Decided by |
| --- | --- | --- | --- |
| Control port | `zlrs-control-uart` / `zlrs-control-cdc` | control | devicetree (compile time) |
| Data port | `zlrs-data-uart` / `zlrs-data-cdc` | data | devicetree (compile time) |
| Link port | `zlrs-link-uart` / `zlrs-link-cdc` | control or data | runtime, default from devicetree |
| Config channel | `zlrs-config-cdc` | configuration protocol | fixed; never carries link traffic |

Rules:

- A board has either fixed-role ports or one link port, never both: mixing the
  two means the same hardware has a decided and an undecided port at once.
- `*-uart` and `*-cdc` are the two endpoints of one port. Either may be missing
  (the port then has a single endpoint); the pair never belongs to two ports.
- A link port requires a default role (`zlrs,default-role = "control" | "data"`)
  so the first boot after flashing is defined.
- Two fixed ports means the port-to-link binding is a build-time decision: the
  board designer wires it and the firmware is built for it. One link port means
  the decision moves to runtime configuration.

### 1.2 What the radio design node carries

```dts
zlrs_rf: zlrs-rf {
	compatible = "zlrs,rf-design";
	band = <0>;              /* 0 = LF (410-510 MHz), 1 = HF (850-930 MHz) */
	low-hz = <410000000>;
	high-hz = <510000000>;
};
```

The band and the frequency range are what region validation uses: a region is
accepted when the radio design covers it, not when a board name matches. The
calibration tables (power versus register value, calibration frequencies) are
properties of the same node and belong to the radio design, not to the MCU
board.

## 2. Capabilities: devicetree in, macros out

Software discovers hardware through one header built entirely from devicetree
macros (`app/caps.h`). There is no second place where "what does this board
have" is written down, and no capability is selected by hand in Kconfig.

```c
/* ports */
#define ZLRS_PORT_CTRL_FIXED   (DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart)) || DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc)))
#define ZLRS_PORT_DATA_FIXED   (DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart))    || DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc)))
#define ZLRS_PORT_LINK         (DT_NODE_EXISTS(DT_ALIAS(zlrs_link_uart))    || DT_NODE_EXISTS(DT_ALIAS(zlrs_link_cdc)))
#define ZLRS_PORT_LINK_DEFAULT_ROLE  DT_ENUM_IDX(DT_NODELABEL(zlrs_link), zlrs_default_role)
#define ZLRS_PORT_CONFIG       DT_NODE_EXISTS(DT_ALIAS(zlrs_config_cdc))

/* peripherals */
#define ZLRS_HAS_BUTTON        DT_NODE_EXISTS(DT_ALIAS(zlrs_button))
#define ZLRS_HAS_LED           DT_NODE_EXISTS(DT_ALIAS(zlrs_led))
#define ZLRS_HAS_WDT           DT_NODE_EXISTS(DT_NODELABEL(watchdog0))
#define ZLRS_HAS_NVS           IS_ENABLED(CONFIG_NVS)
#define ZLRS_HAS_USB           IS_ENABLED(CONFIG_ZLRS_USB)

/* radio design */
#define ZLRS_RF_BAND           DT_PROP(DT_NODELABEL(zlrs_rf), band)
#define ZLRS_RF_LOW_HZ         DT_PROP(DT_NODELABEL(zlrs_rf), low_hz)
#define ZLRS_RF_HIGH_HZ        DT_PROP(DT_NODELABEL(zlrs_rf), high_hz)
```

Three consequences:

1. Feature switches are gated by capabilities, and a contradiction is a build
   error, not a runtime surprise:
   `#if IS_ENABLED(CONFIG_ZLRS_LINK_DATA) && !ZLRS_PORT_DATA_FIXED && !ZLRS_PORT_LINK → #error`.
2. The firmware prints one capability line at boot — `caps: ports=ctrl,data,cfg
   usb=y btn=y led=y rf=LF(410-510M)` — so the user and CI can see what the
   build believes the hardware is.
3. Board names never appear in `app/` or `core/`. CI greps for that.

## 3. Software layers and where files go

| Directory | Contains | Must not |
| --- | --- | --- |
| `core/` | pure C feature core: protocol, link layer, CRSF, config protocol, region tables, LED patterns | include Zephyr headers, know about boards |
| `app/` | the Zephyr application: capability header, ports, radio adaptation, USB device stack, NVS storage, watchdog, button, LED output, superframe, reporting, shell | contain protocol logic that belongs in `core/`, branch on board names |
| `drivers/` | Zephyr drivers we own, bound through devicetree | be a place for application logic |
| `boot/` | code that runs inside the bootloader image | depend on application code |
| `boards/` | one directory per hardware combination: devicetree, defconfig, README with the pin/alias table | contain C |
| `tools/` | host-side helpers: config, recovery, update, bring-up self-test, UF2 packaging | be needed to build firmware |
| `tests/host/` | host build of `core/` plus the two-node radio simulation | need hardware |
| `doc/` | design documents and the documentation index | duplicate facts owned by another document |

The `core/` boundary is what makes the protocol testable on a host, so it is
enforced mechanically (section 6).

## 4. Compile time, runtime, and who decides what

| Decision | When | How |
| --- | --- | --- |
| Which links a build supports | compile time | Kconfig feature switches, gated by capabilities |
| Which port carries which link on a two-port board | compile time | devicetree aliases (`zlrs-control-*`, `zlrs-data-*`) |
| Which link the single port carries | runtime, default at compile time | config protocol, stored in NVS |
| Role TX / RX | runtime, default at compile time | config protocol, stored in NVS |
| Security profile `P0` / `P1` / `P2` | runtime, default at compile time | config protocol, stored in NVS; declared in `SYNC`, both ends must agree |
| USB presence, LED, button, watchdog | compile time | capability macros |
| Region, power ceiling, rate tier, frame rate, ARQ, baud rate, failsafe values, pairing | runtime | config protocol, stored in NVS |

Two properties follow from this split and are worth stating explicitly:

- **One image, both roles.** A field is flashed with the same file and roles are
  assigned during configuration.
- **Defaults are build-time, behaviour is field-time.** Changing how a deployed
  device behaves never requires a new build, only a configuration change.

## 5. Ports constrain features

| The hardware provides | The software may do | The software must refuse |
| --- | --- | --- |
| two ports | control link plus data link, or both ports on one link | a third link |
| one port | one link, role chosen at runtime | two simultaneous links |
| a config CDC | configuration protocol, config mode, recovery entry | using it as a link endpoint |
| a button | long press to pair | pairing without configuration access when there is no button |
| a status LED | the LED language | — |
| NVS | keys, pairing, failsafe, baud rate, region, power | **required**: builds without NVS are rejected, security and regulation need persistence |
| a hardware watchdog | reset on a hang, rollback assistance | — |

The configuration protocol grows three commands to match section 4:
`set-role tx|rx`, `set-profile p0|p1|p2` and, on single-port hardware,
`set-port-role control|data`. All three take effect on the next boot.

## 6. Gates that keep the tree honest

| Gate | Where | What it catches |
| --- | --- | --- |
| `clang-format --dry-run --Werror` over `git ls-files '*.c' '*.h'` | CI, `build.yml` | style drift |
| host tests with ASan + UBSan | CI, `build.yml` | protocol and link regressions |
| script self-tests | CI, `build.yml` | framing drift between tools and firmware |
| build matrix: both sub-G SKUs × plain, bench switches, single-link cuts, LoRa, racing, MCUboot | CI, `firmware.yml` | Kconfig and devicetree combinations that do not compile |
| `core/` contains no `#include <zephyr/` | CI | platform code leaking into the pure C core |
| `app/` contains no `CONFIG_BOARD_*` | CI | software branching on board identity |
| capability line versus board README | review | hardware that is wired but not declared, or the reverse |

## 7. Adding hardware

1. Copy the closest board directory under `boards/<vendor>/<board>/`.
2. Fill in the devicetree: MCU references, the radio node, the `zlrs_rf` design
   node (band, frequency range, calibration tables), and the aliases for the
   ports this hardware has.
3. Write the board README with the pin and alias table and the capability line
   the firmware prints.
4. Add the variant to the CI matrix.
5. If the radio chip is new, add a driver under `drivers/` that implements the
   same interface the existing radio driver does; `core/` does not change.

No application code is expected to change. If it does, a capability is missing
from section 2 and belongs there.
