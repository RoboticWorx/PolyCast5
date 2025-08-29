Important Note: READ BELOW
====================

An external patch was applied to the ESP-IDF framework in order to be able to successfully initialize and deinitialize NimBLE Bluetooth as a HID. **Without this patch, if you initialize, deinitialize, then reinitialize Bluetooth, the 2nd initialization will not work and you will be unable to successfully send commands to whichever connected device.**

Depending on if the patch was applied to your downloaded ESP-IDF release or not, you may have to apply it yourself to build the proper code.

The issue can be found below with the `.patch` file toward the bottom of the issue along with some instructions on how to apply it:
https://github.com/espressif/esp-idf/issues/17493