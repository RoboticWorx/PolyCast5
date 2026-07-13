#ifndef LORA_MESHTASTIC_PORTAL_HTML_H
#define LORA_MESHTASTIC_PORTAL_HTML_H

// Single-page HTML UI served by the Meshtastic web portal (defined in
// lora_meshtastic_portal_html.c). The page is served in two blobs with a
// region-dependent RF-info banner emitted between them at runtime, so HEAD
// (LORA_MESHTASTIC_PORTAL_HTML) ends just after the header and TAIL
// (LORA_MESHTASTIC_PORTAL_HTML_TAIL) begins at the PSK banner.
extern const char *LORA_MESHTASTIC_PORTAL_HTML;
extern const char *LORA_MESHTASTIC_PORTAL_HTML_TAIL;

#endif // LORA_MESHTASTIC_PORTAL_HTML_H
