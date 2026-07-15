# libnet80211.a Patch

This README explains how to apply a pre-patched `libnet80211.a` file to your ESP-IDF v6.0.1 installation for the ESP32-C5 target. This patch is required to bypass internal restrictions in the Wi-Fi stack that prevent sending certain raw IEEE 802.11 frames, such as deauthentication (deauth) frames or beacons. It is intended for education, authorized Wi-Fi penetration testing, or custom protocol development.

## Why This Patch Is Needed

The ESP-IDF Wi-Fi driver for ESP32-C5 (and other RISC-V-based chips) includes a closed-source sanity check (`ieee80211_raw_frame_sanity_check`) inside `libnet80211.a`. This check blocks "unsupported" frame types, including deauthentication frames (subtype 0x0C). When you attempt to send these frames using `esp_wifi_80211_tx`, you get errors such as:

```text
E (x) wifi:unsupport frame type: 0c0
E (x) esp_wifi_80211_tx failed: ESP_ERR_INVALID_ARG
```

The pre-patched `libnet80211.a` file attached in `components/wifi/patch` allows subtype 0x0C frames (0xC0 shifted) to pass this sanity check and therefore be sent. This is done by switching `bVar6 == 0xd0` to `bVar6 == 0xc0` in `ieee80211_raw_frame_sanity_check`.

**Warning: This patch is unofficial and not supported by Espressif**. It may cause instability, crashes, or violate local wireless regulations (e.g., FCC rules on intentional interference). Use **only** for authorized testing on networks you own or have explicit permission to test. Always test in an isolated environment.

## Prerequisites

- ESP-IDF v6.0.1 installed (may work for other versions, but is untested).
- Your project is configured for the **esp32c5** target (should be by default).
- The patched `libnet80211.a` file (provided).
- Administrative access to overwrite files in your ESP-IDF installation directory.

## Step-by-Step

1. **Locate Your ESP-IDF Installation Path**  
    May be something like `C:\esp\.espressif\v6.0.1\esp-idf`.

2. **Navigate to the Original `libnet80211.a` File**  
    Located in `YOUR_IDF_PATH\components\esp_wifi\lib\esp32c5`.

3. **Backup the Original**  
    Make a copy of the original `libnet80211.a` and put it somewhere safe.

4. **Replace with the Patched Library**  
    Overwrite the existing `libnet80211.a` in your installation with the provided patched file (`components/wifi/patch/libnet80211.a`).

5. **Full Clean and Rebuild Your Project**  
    Navigate to your project directory and run:

    ```text
    idf.py fullclean
    idf.py build
    ```

    This step ensures no cached objects from the old library are used.

6. **Flash and Test**

    ```text
    idf.py -p YOUR_PORT flash
    ```

    Your deauth function should now succeed without errors. Should you ever want to revert, simply replace the patched `libnet80211.a` with your saved original or re-download it from the official [ESP-IDF repository](https://github.com/espressif/esp-idf).

## Legal and Ethical Reminder

Deauthentication attacks disrupt Wi-Fi networks and are illegal in most jurisdictions without explicit authorization. This patch is provided for educational and authorized security testing purposes only. The author and contributors assume no liability for misuse.

Use responsibly and ethically.

## Disassembled

```c
undefined4
.text.ieee80211_raw_frame_sanity_check(uint param_1,byte *param_2,uint param_3,int param_4)

{
  byte bVar1;
  undefined4 uVar2;
  int iVar3;
  int iVar4;
  byte bVar5;
  byte bVar6;
  undefined1 auStack_28 [8];
  
                    /* .text.ieee80211_raw_frame_sanity_check Size: 0x212 */
  if (param_2 == (byte *)0x0) {
    FUN_ram_00003498(1,0x40,1,0);
    return 0x102;
  }
  if ((param_3 < 0x18) || (0x5dc < (int)param_3)) {
    FUN_ram_000034da(1,0x40,1,0);
    return 0x102;
  }
  (**(code **)(_switchdataD_ram:00000000 + 0x54))(_switchdataD_ram:00000000);
  iVar4 = iRam00000010;
  if (((param_1 != 0) && (iVar4 = iRam00000014, param_1 != 1)) || (iVar4 == 0)) {
    FUN_ram_00003524(1,0x40,1,0,param_1);
    uVar2 = 0x3004;
    goto LAB_ram_0000352e;
  }
  bVar1 = *param_2;
  bVar5 = bVar1 & 0xc;
  bVar6 = bVar1 & 0xf0;
  if ((param_2[1] & 0x40) == 0) {
    if (bVar5 == 8) {
      if (-1 < (char)bVar6) goto LAB_ram_000035d8;
      bVar5 = 8;
    }
    else if (((bVar1 & 0xc) == 0) &&
            (((bVar6 == 0x80 || ((bVar1 & 0xe0) == 0x40)) || (bVar6 == 0xd0)))) {
LAB_ram_000035d8:
      FUN_ram_000035de(param_1 & 0xff,auStack_28);
      iVar3 = FUN_ram_000035ee(auStack_28,param_2 + 10,6);
      if (param_1 == 0) {
        if ((*(int *)(iVar4 + 0xe4) == 0) ||
           (iVar4 = FUN_ram_00003608(*(int *)(iVar4 + 0xe4) + 4,param_2 + 4,6), iVar4 != 0))
        goto LAB_ram_00003612;
      }
      else {
        iVar4 = FUN_ram_00003626(param_2 + 4);
        if (iVar4 == 0) goto LAB_ram_00003612;
      }
      if (iVar3 != 0) {
LAB_ram_00003612:
        (**(code **)(_switchdataD_ram:00000000 + 0x58))(_switchdataD_ram:00000000);
        return 0;
      }
      if ((param_4 != 0) && ((param_2[1] & 0x3c) == 0)) {
        if ((*param_2 & 0xc) != 8) goto LAB_ram_00003612;
        bVar5 = param_2[1] & 3;
        if (param_1 == 0) {
          if (bVar5 == 1) goto LAB_ram_00003612;
        }
        else if (bVar5 == 2) goto LAB_ram_00003612;
      }
      goto LAB_ram_0000356e;
    }
    FUN_ram_000035b4(1,0x40,1,0,bVar5);
  }
  else {
LAB_ram_0000356e:
    FUN_ram_00003576(1,0x40,1,0);
  }
  uVar2 = 0x102;
LAB_ram_0000352e:
  (**(code **)(_switchdataD_ram:00000000 + 0x58))(_switchdataD_ram:00000000);
  return uVar2;
}
```

```c
int .text.esp_wifi_80211_tx(uint param_1,undefined4 param_2,int param_3,int param_4)

{
  char cVar1;
  undefined1 uVar2;
  int iVar3;
  undefined1 *puVar4;
  int iVar5;
  uint uVar6;
  int iVar7;
  uint *puVar8;
  int iStack_3c;
  int iStack_38;
  undefined4 uStack_34;
  
                    /* .text.esp_wifi_80211_tx Size: 0x1aa */
  iVar3 = FUN_ram_000036a2();
  if (iVar3 != 0) {
    return iVar3;
  }
  (**(code **)(_switchdataD_ram:00000000 + 0x54))(_switchdataD_ram:00000000);
  iVar3 = FUN_ram_000036c8(param_2,1,param_3);
  if (iVar3 == 0) {
    (**(code **)(_switchdataD_ram:00000000 + 0x58))(_switchdataD_ram:00000000);
    return 0x101;
  }
  puVar4 = (undefined1 *)FUN_ram_000036fc();
  iVar5 = FUN_ram_0000370a(*puVar4);
  *(int *)(iVar3 + 0x18) = param_3 + -0x18;
  puVar8 = *(uint **)(iVar3 + 0x38);
  *(undefined2 *)(iVar3 + 0x14) = 0x18;
  *puVar8 = *puVar8 | 0x4000;
  uVar6 = FUN_ram_0000372e();
  puVar8[7] = uVar6;
  uVar2 = FUN_ram_00003740(param_1 & 0xff);
  iStack_3c = 0;
  *(undefined1 *)(*(int *)(iVar3 + 0x38) + 0xc) = uVar2;
  iStack_38 = 0;
  uStack_34 = 0;
  FUN_ram_0000375a(param_1 & 0xff,&iStack_3c);
  puVar8 = *(uint **)(iVar3 + 0x38);
  iVar7 = iStack_38;
  if (iStack_38 == 0) {
    if (iVar5 != 2) {
      *(undefined1 *)(puVar8 + 3) = 0;
      goto LAB_ram_0000376e;
    }
    iVar7 = 0xb;
  }
  *(char *)(puVar8 + 3) = (char)iVar7;
LAB_ram_0000376e:
  if (iStack_3c == 6) {
    *puVar8 = *puVar8 | 0x80000000;
    *(byte *)((int)puVar8 + 0x2f) =
         (byte)(((uStack_34 & 0xff) + 6 & 0xf) << 3) | *(byte *)((int)puVar8 + 0x2f) & 0x87;
    if (uStack_34._1_1_ != '\0') {
      *(byte *)((int)puVar8 + 0x31) = *(byte *)((int)puVar8 + 0x31) | 0x80;
    }
  }
  else if (iStack_3c == 7) {
    *puVar8 = *puVar8 | 0x1000000;
  }
  cVar1 = cRam000002c6;
  if (param_1 == 0) {
    cVar1 = cRam000002ca;
  }
  puVar8[2] = (uint)(cVar1 == '\x02') << 0xf | puVar8[2] & 0xffff7fff;
  if (param_4 != 0) {
    *puVar8 = *puVar8 | 1;
  }
  puVar8[4] = puVar8[4] & 0xfff3ffff | (param_1 & 3) << 0x12;
  puVar8[5] = 0x100;
  FUN_ram_000037f2(iVar3);
  (**(code **)(_switchdataD_ram:00000000 + 0x58))(_switchdataD_ram:00000000);
  return 0;
}
```

[Credit for v5.5.2 patched `libnet80211.a` file for which this is based](https://github.com/AnvilBrain/esp32-c5-dualband-deauther)

For developers:

```bash
ar x libnet80211.a // Extract archive
ieee80211_output.o // To patch
ar rcs libnet80211_patched.a *.o // Rebuild archive
```
