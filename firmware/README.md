# Firmware

`targets/x86_64/ramdisk/ax101.fw` is Intel's `iwlwifi-so-a0-hr-b0-77.ucode`
from the linux-firmware repository, unchanged (renamed to fit the RAM disk's
8.3 file names). The Wi-Fi driver (`src/impl/x86_64/drivers/iwlwifi.c`) loads
it into the AX101 card. It is redistributed under Intel's licence, in
`LICENCE.iwlwifi_firmware` here.
