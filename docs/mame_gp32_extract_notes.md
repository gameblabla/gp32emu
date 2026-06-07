# MAME GP32

Used MAME resources

Relevant MAME files:

- `src/mame/gamepark/gp32.cpp`
- `src/mame/gamepark/gp32.h`
- `src/devices/machine/smartmed.cpp`
- `src/devices/machine/smartmed.h`
- `src/devices/machine/nandflash.cpp`
- `src/devices/machine/nandflash.h`
- `src/devices/cpu/arm7/*` for ARM7/ARM9 behavior reference
- `hash/gp32.xml` for SmartMedia image sizes and BIOS metadata

GP32 memory map:

| Range | Device |
|---|---|
| `00000000-0007ffff` | BIOS ROM |
| `0c000000-0c7fffff` | SDRAM, 8 MiB |
| `14000000-1400003b` | memory controller |
| `14200000-1420005b` | USB host |
| `14400000-14400017` | interrupt controller |
| `14600000-1460007b` | DMA |
| `14800000-14800017` | clock/power |
| `14a00000-14a003ff` | LCD controller |
| `14a00400-14a007ff` | LCD palette |
| `15000000-1500002b` | UART0 |
| `15004000-1500402b` | UART1 |
| `15100000-15100043` | PWM timers |
| `15200140-152001fb` | USB device |
| `15300000-1530000b` | watchdog |
| `15400000-1540000f` | IIC |
| `15508000-15508013` | IIS |
| `15600000-1560005b` | GPIO |
| `15700040-1570008b` | RTC |
| `15800000-15800007` | ADC |
| `15900000-15900017` | SPI |
| `15a00000-15a0003f` | MMC |

GPIO wiring:

- `PBCON bit 0`: SmartMedia read direction.
- `PBDAT bits 0-7`: SmartMedia data bus; `bits 8-15`: active-low buttons R/L/D-pad/B/A.
- `PDDAT bits 6-9`: SmartMedia WP, chip select, read strobe, busy.
- `PEDAT bits 2-5`: SmartMedia present, write strobe, address latch, command latch; `bits 6-7`: active-low START/SELECT.
