# libnet80211.a Patch

This README explains how to apply a pre-patched `libnet80211.a` file to your ESP-IDF v6.0 installation for the ESP32-C5 target. This patch is required to bypass internal restrictions in the Wi-Fi stack that prevent sending certain raw IEEE 802.11 frames, such as deauthentication (deauth) frames or beacons. It is intended for education, authorized Wi-Fi penetration testing, or custom protocol development.

## Why This Patch Is Needed

The ESP-IDF Wi-Fi driver for ESP32-C5 (and other RISC-V-based chips) includes a closed-source sanity check (`ieee80211_raw_frame_sanity_check`) inside `libnet80211.a`. This check blocks "unsupported" frame types, including deauthentication frames (subtype 0x0C). When you attempt to send these frames using `esp_wifi_80211_tx`, you get errors such as:

```text
E (x) wifi:unsupport frame type: 0c0
E (x) esp_wifi_80211_tx failed: ESP_ERR_INVALID_ARG
```

The pre-patched `libnet80211.a` file attached in `components/wifi/patch` allows subtype 0x0C frames (0xC0 shifted) to pass this sanity check and therefore be sent. This is done by switching `bVar7 == 0xd0` to `bVar7 == 0xc0` in `ieee80211_raw_frame_sanity_check`.

**Warning: This patch is unofficial and not supported by Espressif**. It may cause instability, crashes, or violate local wireless regulations (e.g., FCC rules on intentional interference). Use **only** for authorized testing on networks you own or have explicit permission to test. Always test in an isolated environment.

## Prerequisites

- ESP-IDF v6.0 installed (may work for other versions, but is untested).
- Your project is configured for the **esp32c5** target (`idf.py set-target esp32c5`).
- The patched `libnet80211.a` file (provided).
- Administrative access to overwrite files in your ESP-IDF installation directory.

## Step-by-Step

1. **Locate Your ESP-IDF Installation Path**  
    May be something like `C:\esp\.espressif\v6.0\esp-idf`.

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
undefined4 ieee80211_raw_frame_sanity_check(uint param_1,byte *param_2,uint param_3,int param_4)
{
  byte bVar1;
  undefined4 uVar2;
  int iVar3;
  int iVar4;
  char *pcVar5;
  byte bVar6;
  byte bVar7;
  undefined1 auStack_28 [8];
  
  if (param_2 == (byte *)0x0) {
    wifi_log(1,0x40,1,"invalid buffer");
    return 0x102;
  }
  if ((param_3 < 0x18) || (0x5dc < (int)param_3)) {
    wifi_log(1,0x40,1,"invalid buffer length: %d");
    return 0x102;
  }
  (**(code **)(_g_osi_funcs_p + 0x54))(_g_wifi_global_lock);
  iVar4 = _s_netstack_free;
  if (((param_1 != 0) && (iVar4 = _g_osi_funcs_p, param_1 != 1)) || (iVar4 == 0)) {
    wifi_log(1,0x40,1,"invalid interface %d",param_1);
    uVar2 = 0x3004;
    goto .L1129;
  }
  bVar1 = *param_2;
  bVar6 = bVar1 & 0xc;
  bVar7 = bVar1 & 0xf0;
  if ((param_2[1] & 0x40) == 0) {
    if (bVar6 == 8) {
      if (-1 < (char)bVar7) goto .L1133;
      bVar6 = 8;
      pcVar5 = "unsupport QoS frame type: %x%x";
    }
    else {
      if (((bVar1 & 0xc) == 0) && (((bVar7 == 0x80 || ((bVar1 & 0xe0) == 0x40)) || (bVar7 == 0xd0) ))
         ) {
.L1133:
        wifi_get_macaddr(param_1 & 0xff,auStack_28);
        iVar3 = memcmp(auStack_28,param_2 + 10,6);
        if (param_1 == 0) {
          if ((*(int *)(iVar4 + 0xe4) == 0) ||
             (iVar4 = memcmp((void *)(*(int *)(iVar4 + 0xe4) + 4),param_2 + 4,6), iVar4 != 0))
          goto .L1135;
        }
        else {
          iVar4 = cnx_node_search(param_2 + 4);
          if (iVar4 == 0) goto .L1135;
        }
        if (iVar3 != 0) goto .L1135;
        if (param_4 == 0) {
          pcVar5 = "en_sys_seq should be true to avoid side-effect to WiFi connection";
        }
        else if ((param_2[1] & 0x3c) == 0) {
          if ((*param_2 & 0xc) != 8) {
.L1135:
            (**(code **)(_g_osi_funcs_p + 0x58))(_g_wifi_global_lock);
            return 0;
          }
          bVar6 = param_2[1] & 3;
          if (param_1 == 0) {
            if (bVar6 == 1) goto .L1135;
            pcVar5 = "invalid frame control, sta->ap ToDS should be 1, FromDS should be 0";
          }
          else {
            if (bVar6 == 2) goto .L1135;
            pcVar5 = "invalid frame control, ap->sta ToDS should be 0, FromDS should be 1";
          }
        }
        else {
          pcVar5 = "invalid frame control, retry/power/frag/more data bit should not set";
        }
        goto .L1149;
      }
      pcVar5 = "unsupport frame type: %x%x";
    }
    wifi_log(1,0x40,1,pcVar5,bVar6);
  }
  else {
    pcVar5 = "invalid frame control, unsupport crypto frame";
.L1149:
    wifi_log(1,0x40,1,pcVar5);
  }
  uVar2 = 0x102;
.L1129:
  (**(code **)(_g_osi_funcs_p + 0x58))(_g_wifi_global_lock);
  return uVar2;
}
```

```c
int esp_wifi_80211_tx(uint param_1,undefined4 param_2,int param_3,int param_4)
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
  
  iVar3 = ieee80211_raw_frame_sanity_check();
  if (iVar3 != 0) {
    return iVar3;
  }
  (**(code **)(_g_osi_funcs_p + 0x54))(_g_wifi_global_lock);
  iVar3 = ic_ebuf_alloc(param_2,1,param_3);
  if (iVar3 == 0) {
    (**(code **)(_g_osi_funcs_p + 0x58))(_g_wifi_global_lock);
    return 0x101;
  }
  puVar4 = (undefined1 *)chm_get_current_channel();
  iVar5 = chm_get_band_from_chan(*puVar4);
  *(int *)(iVar3 + 0x18) = param_3 + -0x18;
  puVar8 = *(uint **)(iVar3 + 0x38);
  *(undefined2 *)(iVar3 + 0x14) = 0x18;
  *puVar8 = *puVar8 | 0x4000;
  uVar6 = ic_get_default_sched();
  puVar8[7] = uVar6;
  uVar2 = ic_get_80211_tx_rate(param_1 & 0xff);
  iStack_3c = 0;
  *(undefined1 *)(*(int *)(iVar3 + 0x38) + 0xc) = uVar2;
  iStack_38 = 0;
  uStack_34 = 0;
  ic_get_80211_tx_rate_config(param_1 & 0xff,&iStack_3c);
  puVar8 = *(uint **)(iVar3 + 0x38);
  iVar7 = iStack_38;
  if (iStack_38 == 0) {
    if (iVar5 != 2) {
      *(undefined1 *)(puVar8 + 3) = 0;
      goto .L1157;
    }
    iVar7 = 0xb;
  }
  *(char *)(puVar8 + 3) = (char)iVar7;
.L1157:
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
  cVar1 = DAT_ram_000162ca;
  if (param_1 == 0) {
    cVar1 = DAT_ram_000162ce;
  }
  puVar8[2] = (uint)(cVar1 == '\x02') << 0xf | puVar8[2] & 0xffff7fff;
  if (param_4 != 0) {
    *puVar8 = *puVar8 | 1;
  }
  puVar8[4] = puVar8[4] & 0xfff3ffff | (param_1 & 3) << 0x12;
  puVar8[5] = 0x100;
  ieee80211_post_hmac_tx(iVar3);
  (**(code **)(_g_osi_funcs_p + 0x58))(_g_wifi_global_lock);
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
