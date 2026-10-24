# UsbNabu

USB Type-C drivers for the Xiaomi Pad 5 (nabu) running Windows on ARM.

They replace Qualcomm's Type-C drivers, which on this tablet cannot charge and act as a USB host in
the same session. With UsbNabu:

- USB PD charging works, within Xiaomi's own charging limits for this tablet (at most 18 W from a
  9 V charger).
- OTG host mode works (mouse, keyboard, flash drive, hubs).
- Charging and host mode work together, in any plug order, without a reboot, including PD
  pass-through hubs.
- The tablet takes the sink role against a PC or Mac and charges from it.

## Drivers

| Driver | Devices | Role |
|---|---|---|
| `NabuTypeC` | `ACPI\QCOM0582`, `ACPI\QCOM057D` | Type-C port controller and USB PD PHY on the PM8150B, through Windows' UcmTcpciCx framework |
| `NabuSrcGuard` | `ACPI\QCOM05E5`, `ACPI\QCOM057D`, USB function child | Filter: charging limits from the PD contract, the 9 V / 3 A sink capability, the function port's missing `_UPC` |

## Charging limits

UsbNabu follows Xiaomi's charging limits for this tablet:

- From a 9 V charger the input is limited to 2.0 A (18 W). At 5 V it takes what the charger offers.
- Charge current, voltage and the temperature limits are Xiaomi's values for this battery.

Releases before 1.0.15.21-r4 drew up to about 25 W. This one is simply closer to stock.

Qualcomm's driver packages stay installed. Uninstalling UsbNabu hands the devices back to them.

## Install

Requirements: Windows on ARM on the Xiaomi Pad 5 with the Windows On Andromeda platform drivers. The
drivers are test-signed, so test signing must be on (and Secure Boot off).

1. Turn on test signing in an administrator terminal, then restart:

   ```
   bcdedit /set testsigning on
   ```

2. Download `nabu-usb-<version>.zip` from [Releases](../../releases) and extract it.
3. Open PowerShell as administrator in the extracted folder and run:

   ```
   Set-ExecutionPolicy -Scope Process Bypass
   .\install.ps1
   ```

   The script trusts the "Nabu USB Drivers" certificate and installs both drivers.
4. Restart.

Tip: Windows cannot resume from hibernate on this platform. `powercfg /h off` avoids a failed resume
when shutting down with Fast Startup.

## Uninstall

In the same folder, as administrator: `.\uninstall.ps1`, then restart. Qualcomm's drivers take over
again, and the certificate is removed unless another Nabu driver package still uses it.

## Source

`drivers/nabu-typec/` and `drivers/nabu-srcguard/` hold the driver sources (C++17, KMDF 1.33,
ARM64) and their INF files; `drivers/shared/` holds a kernel-link header both drivers include. Signed
packages are published under Releases.

## Thanks

- Windows On Andromeda (Gustave Monce and contributors): the
  platform drivers and UEFI that Windows on the Xiaomi Pad 5 runs on.
- The Linux kernel's Qualcomm PMIC Type-C drivers (`qcom_pmic_typec_port.c`,
  `qcom_pmic_typec_pdphy.c`, Bryan O'Donoghue / Linaro): the behavioural reference for the PM8150B
  Type-C port and PD PHY. No code was copied.
- Xiaomi's open-source kernel for the Pad 5: the charging limits, the JEITA table and the PM8150B
  charger register map.

## Disclaimer

These drivers are provided as is, without any warranty. Use them at your own risk: the authors take
no responsibility for any damage to your device, battery or data.

## License

GPL-2.0, see [LICENSE](LICENSE).
