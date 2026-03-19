#include "ai_prompts.h"

// AI dev prompt string for BLE HID keyboard script generation
// Note: this is the compiled default: runtime override may be loaded from NVS via the web portal
const char AI_PROMPT_AUTOKEY[] =
  "Generate a BLE HID keystroke script for Windows 11 (US), Chrome installed.\n"
  "Output ONLY allowed tokens as ONE LINE. End with !END!\n"
  "\n"
  "Allowed tokens:\n"
  "<delay=MS> <hold:KEY=MS> <enter> <tab> <esc> <space> <bs> <del>\n"
  "<ctrl> <shift> <alt> <opt> <win> <cmd> <up> <down> <left> <right> <pgup> <pgdn>\n"
  "<f1>-<f12>\n"
  "Combos with '+' are also allowed: e.g. <ctrl+c> <ctrl+shift+v> <alt+tab> <win+r> <win+s> <ctrl+t> <ctrl+l> etc.\n"
  "\n"
  "Rules:\n"
  "- Execute every requested step in order. Do only what was asked.\n"
  "- Split steps on: then/and then/after that/next/etc.\n"
  "- ACTION=UI control (open/close/switch/navigate/scroll/wait/lock/play/pause).\n"
  "- TYPE=write content. Ambiguous -> TYPE. Never skip explicit ACTIONs.\n"
  "- Never type narration (e.g. \"open chrome\"). Never output real newlines.\n"
  "- No quotes/JSON/markdown. Use <enter> instead of newlines.\n"
  "- Web/search only if explicitly asked. tell/explain/list/write -> TYPE the answer.\n"
  "- Never drop later ACTION steps; long TYPE content: summarize.\n"
  "- No app is open unless implied.\n"
  "- If a different OS is specified, create commands for that OS instead..\n"
  "\n"
  "Delays: default <delay=500>; app/page load <delay=1000>; user wait N sec -> <delay=N000>.\n"
  "App launch: <win+s><delay=500>APP<delay=500><enter><delay=1000>\n"
  "App launch iOS specified: <cmd+space><delay=500>APP<delay=500><enter><delay=1000>\n";

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
  "Respond with ONLY the INTEGER global_index of the best match. If none match, respond with -1."
  "\n"
  "Do not output any other text.";

// AI dev prompt string for custom command label location
const char AI_PROMPT_CUSTOM[] =
  "You are selecting ONE saved custom command entry from a list."
  "\n"
  "The user described what they want to do. Pick the BEST matching entry."
  "\n"
  "Each entry line is: global_index|category_name|label"
  "\n"
  "Respond with ONLY the INTEGER global_index of the best match. If none match, respond with -1."
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
