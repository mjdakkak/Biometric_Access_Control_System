# Prototype pin and power record

This records the supplied firmware and the discussed bench arrangement. It is not a substitute for identifying connector orientation, signal voltage, component pinouts, or supply ratings on the actual hardware. Build with power disconnected. Do not use the previously generated incorrect relay illustrations.

## Board

Freenove ESP32-S3-WROOM PCB V1.2, module marking N16R8; camera ribbon OV3660. Keep the board camera connection. Use the documented header GPIO labels, not physical pin counts. Leave the onboard microSD slot empty because GPIO38/39/40 are used by UARTs.

| Signal | ESP32 GPIO |
|---|---:|
| Nextion TX -> ESP RX | 39 |
| Nextion RX <- ESP TX | 40 |
| RC522 SS/SDA | 21 |
| RC522 RST | 47 |
| RC522 SCK | 41 |
| RC522 MISO | 42 |
| RC522 MOSI | 2 |
| AS608 TX -> ESP RX | 1 |
| AS608 RX <- ESP TX | 38 |
| Proposed lock-interface output | 14 |

UART rates: Nextion 115200, fingerprint 57600. RC522 IRQ unused. Camera GPIOs remain as defined in HardwareConfig.h. GPIO41/42 cannot also be occupied by an external JTAG connection in this allocation.

## Supplies

Observed by the user: HW-131 unloaded rails about 3.320 V/5.112 V; approximately 5.014 V at the operating Nextion. This does not establish combined-load stability, thermal margin or a certified current rating.

- Computer USB powers the Freenove/camera.
- HW-131 3.3 V is the proposed RC522/AS608 rail; 5 V supplies Nextion and, if capacity allows, relay electronics.
- Join logic grounds, including Freenove GND. Do not parallel HW-131 positive outputs with the Freenove's powered supply outputs; never join 5 V and 3.3 V.
- A separately verified 12.0 V adapter supplies only the lock through isolated relay contacts. Its negative returns directly to that adapter, not through a breadboard or ESP32 ground pin.
- Keep 12 V current off solderless breadboards. Use suitable wire, secure insulated joints and appropriate overcurrent protection. Supply-current and coil-duty ratings remain unverified.

The photographed AS608 connector reads V+, TX, RX, GND, TCH, VA, D+, D− with its white socket at the bottom. Trace the actual harness. The discussed UART-only setup uses its 3.3 V main input V+, TX/RX/GND; unused touch/USB pins are not grounds. Confirm specifications for the exact unit.

## Relay-input circuit — electrical netlist only

For a verified low-trigger 5 V module and an identified NPN such as S8050:

- GPIO14 -> 1 kOhm -> base.
- Base -> 10 kOhm -> common logic ground.
- Emitter -> common logic ground.
- Collector -> IN1.
- IN1 -> 10 kOhm -> 5 V (not a direct 5 V wire).
- IN2 -> 10 kOhm -> 5 V for the unused low-trigger channel.
- Module GND -> logic ground. VCC/JD-VCC -> 5 V using the verified supply jumper.

Verify the actual transistor E/B/C pinout; the name alone does not certify lead order. Do not directly wire IN1 to GND or 5 V. With this verified circuit, GPIO HIGH activates the low-trigger relay through the NPN.

## Lock contacts and diode

Identify COM/NO/NC by the board symbols and de-energized/energized contact checks with external contact power removed. Do not rely solely on photo position.

Verified 12 V positive -> correctly rated fuse near source -> COM1. NO1 -> lock positive. Lock negative -> adapter negative. NC1 unused. This assumes a verified power-to-unlock coil.

1N4007 goes in parallel across the lock: band/cathode -> lock positive; unbanded/anode -> lock negative. Mount near the coil and insulate. It does not replace a fuse. Do not put 12 V on module VCC/IN1. The NPN switches the relay input, not the solenoid current.

Leave KIOSK_ENABLE_LOCK=0 and the adapter physically unplugged until circuit, polarity, fuse/current limit, coil duty and return behavior are verified. The current three-second software pulse is not proof of a safe coil rating. A disabled GPIO does not physically isolate a miswired relay.
