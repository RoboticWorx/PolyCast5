#ifndef STUB_ESP_ATTR_H
#define STUB_ESP_ATTR_H
/* Host stand-in for esp_attr.h. Defined empty, but the point is that a source file must
   still INCLUDE it (directly or via freertos) exactly as it must on target. */
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#endif
