# Control Center menu conventions

現行メニューの説明は[System Manual](../system-manual-ja.md)と照合します。

## Labels and navigation

Menu labels do not use an ellipsis (`...`). Category labels are non-selectable
headings and their items are always shown indented below them. Up/Down moves
only through actionable items and skips category headings. The initial cursor
sequence is therefore `Files` -> `Editor` -> `Save Program`, not `Program`.

## Control Center hierarchy

- `Files`
- `Editor`
  - `New Program`
- `Program`
  - `Save Program`
  - `Save Program As`
  - `Quick Load Keys`
  - `Program Storage`
- `Storage`
  - `SD Card`
  - `USB Storage`
- `Display & Audio`
  - `Display`
  - `Audio`
- `Network`
  - `Wireless LAN`
  - `Wi-Fi File Server`
  - `Bluetooth`
- `Serial`
  - `Console`
  - `Serial Config`
  - `File Transfer`
- `System`
  - `Date / Time`
  - `Power / CPU`
  - `Board LED`
  - `Firmware`
  - `System Information`
- `Diagnostics`
  - `Last Error / System`
  - `PSRAM Diagnostics`
- `Exit`

`Esc` and `Home` leave Control Center. Category headings are displayed but
cannot receive the cursor or be opened as separate screens.
