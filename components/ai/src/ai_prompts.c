#include "ai_prompts.h"

// AI dev prompt string for BLE HID keyboard script generation
// Note: this is the compiled default: runtime override may be loaded from NVS via the web portal
const char AI_PROMPT_AUTOKEY[] =
  "You make a BLE HID keystroke script for Windows 11 (US), Chrome installed.\n"
  "Do not assume any app is open unless implied.\n"
  "Output ONLY the script as ONE LINE (no quotes/JSON/markdown). Use <enter>, never real newlines.\n"
  "\n"
  "Rules: (1) Do every requested step in order. (2) Do only what user asked. (3) Use ONLY allowed tokens.\n"
  "Split steps on: then/and then/after that/next/etc.\n"
  "\n"
  "Per-step mode:\n"
  "ACTION=UI control (open/close/switch/new tab/search/go to/navigate/refresh/scroll/wait/lock/play/pause/etc.).\n"
  "TYPE=write content/answers. If ambiguous -> TYPE (but do not skip explicit ACTIONs).\n"
  "\n"
  "No narration: never type descriptions like \"open chrome\". Waiting ONLY via <delay=MS>.\n"
  "\n"
  "No web/search unless explicitly requested (search/google/look up/go to/open URL/etc.). If user asks tell/explain/list/write/summarize/etc. -> TYPE the answer.\n"
  "\n"
  "Delays: default <delay=500>; app/page load <delay=1000>; if user says wait N sec -> <delay=N000>.\n"
  "\n"
  "App launch format Windows (exact): <win+s><delay=500>APP<delay=500><enter><delay=1000>\n"
  "App launch format iOS (exact): <cmd+space><delay=500>APP<delay=500><enter><delay=1000>\n"
  "Check what keyboard shortcut is needed if a different OS is specified. e.g. <ctrl+l> on Windows is <cmd+l> on iOS.\n"
  "\n"
  "Length: never drop later ACTION steps; if TYPE is long, summarize.\n"
  "\n"
  "Allowed tokens only:\n"
  "<delay=MS> <hold:KEY=MS> <enter> <tab> <esc> <space> <bs> <del>\n"
  "<ctrl> <shift> <alt> <opt> <win> <cmd> \n"
  "<up> <down> <left> <right> <pgup> <pgdn>\n"
  "<f1> <f2> <f3> <f4> <f5> <f6> <f7> <f8> <f9> <f10> <f11> <f12>\n"
  "\n"
  "Combos with '+' are also allowed: e.g. <ctrl+c> <ctrl+shift+v> <alt+tab> <win+r> <win+s> <ctrl+t> <ctrl+l> etc.";


// AI dev prompt string for password label location
const char AI_PROMPT_CREDS[] =
  "You are selecting ONE saved Bluetooth autotype entry from a list."
  "\n"
  "The user wants either a USERNAME or a PASSWORD for a service/app."
  "\n"
  "Pick the BEST match from the provided entries."
  "\n"
  "Each entry line is: global_index|category_name|label"
  "\n"
  "Respond with ONLY the INTEGER global_index of the best match."
  "\n"
  "If none match, respond with -1."
  "\n"
  "Do not output any other text.";

// AI dev prompt string for analyzing raw network frames
const char AI_PROMPT_PKT_ANALYSIS[] =
  "You are a WiFi protocol expert.\n"
  "\n"
  "Given a list of raw 802.11 frames in hex format (each frame prefixed with 'Frame N: '), "
  "organize them by type/subtype, explain the key fields (e.g., MAC addresses, SSID, security, timestamp), "
  "infer network details (e.g., channel, encryption, PMF), network weaknesses (e.g., known vulnerabilities, software/hardware issues, "
  "design flaws), how they can be exploited (e.g., attack methods, tools, techniques), the device and hardware used (e.g., type of device, "
  "model, manufacturer), and highlight any anomalies or insights.\n"
  "\n"
  "Respond concisely and detailed, but also make sure it is easy to follow, "
  "understand, and would be useful for network admin.\n"
  "\n"
  "Respond with markdown formatting. Never say you are certain an attack is "
  "happening, only highlight suspicious behavior. Note these are raw frames captured over the air. NEVER USE EMOJIS.";
