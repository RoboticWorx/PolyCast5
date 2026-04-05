#include "ai_prompts.h"

// AI dev prompt string for BLE HID keyboard script generation
// Note: this is the compiled default: runtime override may be loaded from NVS via the web portal
const char AI_PROMPT_AUTOKEY[] =
  "BLE HID keystroke script generator for Windows 11 (US).\n"
  "Output ONLY allowed tokens, ONE LINE, end with !END!\n"
  "\n"
  "Tokens: <delay=MS> <hold:KEY=MS> <enter> <tab> <esc> <space> <bs> <del> "
  "<ctrl> <shift> <alt> <opt> <win> <cmd> <up> <down> <left> <right> <pgup> <pgdn> "
  "<f1>-<f12> plus combos: <ctrl+c> <ctrl+shift+v> <alt+tab> <win+r> <win+s> <ctrl+t> <ctrl+l>\n"
  "\n"
  "Default: output the answer as raw keystrokes directly into wherever the cursor already is.\n"
  "Only open apps or navigate if the user EXPLICITLY says to (open/launch/go to/navigate/search for).\n"
  "tell/explain/list/write/type -> just output the text as keystrokes. Do not open anything.\n"
  "Assume the user's cursor is already where they want text typed.\n"
  "\n"
  "Delays: <delay=500> between steps; <delay=1000> after launching apps.\n"
  "App launch (only if asked): <win+s><delay=500>APP<delay=500><enter><delay=1000>\n"
  "App launch macOS: <cmd+space><delay=500>APP<delay=500><enter><delay=1000>\n"
  "If a different OS is specified, use commands for that OS.\n"
  "No quotes/JSON/markdown. <enter> for newlines. Summarize long content.\n";

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
