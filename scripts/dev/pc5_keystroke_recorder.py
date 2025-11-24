# pc5_keystroke_recorder.py - polycast5.com
#
# Records keyboard activity as <down:...>, <up:...>, and <delay=MS> tags
# Compatible with the PolyCast5 ESP32-C5 HID script engine!
#
# - Tracks real overlapping keypresses:
#     <down:q>
#     <delay=80>
#     <down:q + w>
#     <delay=40>
#     <up:q>
#     <delay=60>
#     <up:w>
#
# - Modifiers (ctrl/alt/shift/gui) are independent:
#     <down:ctrl>
#     <down:ctrl + c>
#     <up:c>
#     <up:ctrl>
#
# Hotkeys (NOT recorded into the script):
#   ` (backtick) = toggle pause/resume recording
#   Esc          = stop, save, and print the output
#
# Requires: pip install pynput or python -m pip install pynput 
# Run:      python pc5_keystroke_recorder.py --outfile pc5_recorded_script.txt

import argparse
import time
from pynput import keyboard

# ---------- High-resolution timing helpers ----------

# Grabs a monotonically increasing time in nanoseconds
def now_ns() -> int:
    return time.perf_counter_ns()

# Converts nanoseconds to milliseconds
def ns_to_ms_round_up(ns: int) -> int:
    """Convert ns->ms, rounding up so tiny gaps don't become 0 ms."""

    if ns <= 0:
        return 0
    
    ms = ns // 1_000_000
    if ns % 1_000_000:
        ms += 1

    return ms

# ---------- Key name maps (align with your device grammar) ----------

# Generate tags that the PolyCast5 parser understands
NAMED_KEYS = {
    keyboard.Key.enter: "enter",
    keyboard.Key.tab: "tab",
    keyboard.Key.esc: "esc", # Not recorded; handled as hotkey, but kept for completeness
    keyboard.Key.space: "space",
    keyboard.Key.backspace: "backspace",
    keyboard.Key.delete: "del",
    keyboard.Key.up: "up",
    keyboard.Key.down: "down",
    keyboard.Key.left: "left",
    keyboard.Key.right: "right",
    keyboard.Key.home: "home",
    keyboard.Key.end: "end",
    keyboard.Key.page_up: "pgup",
    keyboard.Key.page_down: "pgdn",
}

# Same idea, just for F1–F12
FUNCTION_KEYS = {
    keyboard.Key.f1: "f1",   keyboard.Key.f2: "f2",   keyboard.Key.f3: "f3",
    keyboard.Key.f4: "f4",   keyboard.Key.f5: "f5",   keyboard.Key.f6: "f6",
    keyboard.Key.f7: "f7",   keyboard.Key.f8: "f8",   keyboard.Key.f9: "f9",
    keyboard.Key.f10: "f10", keyboard.Key.f11: "f11", keyboard.Key.f12: "f12",
}

# Collapse L/R variants to a single logical modifier the PolyCast5 parser understands.
MOD_MAP = {
    keyboard.Key.ctrl:    "ctrl",
    keyboard.Key.ctrl_l:  "ctrl",
    keyboard.Key.ctrl_r:  "ctrl",
    keyboard.Key.shift:   "shift",
    keyboard.Key.shift_l: "shift",
    keyboard.Key.shift_r: "shift",
    keyboard.Key.alt:     "alt",
    keyboard.Key.alt_l:   "alt",
    keyboard.Key.alt_r:   "alt",
    keyboard.Key.cmd:     "gui", # macOS Command key
    keyboard.Key.cmd_l:   "gui",
    keyboard.Key.cmd_r:   "gui",
}


def main():
    ap = argparse.ArgumentParser(
        description="Record keydown/keyup as <down:...>, <up:...> with <delay=MS>."
    )

    # Where to write the final script
    ap.add_argument(
        "--outfile",
        default="pc5_recorded_script.txt",
        help="Output script file",
    )

    # Idle gap threshold before emitting <delay=...>
    ap.add_argument(
        "--delay-threshold",
        type=int,
        default=30,
        help="Idle gap (ms) before emitting <delay=...> (default: 30)",
    )

    args = ap.parse_args()

    # Recorder state
    recording = False
    script_parts: list[str] = []
    last_emit_ns = now_ns()

    pressed_mods: set[str] = set()  # {"ctrl","alt","gui","shift"}

    # Non-mod keys currently held: keyobj -> {"base": "a"}
    pending: dict = {}

    # ---------- Helpers ----------

    def emit_delay_if_needed():
        # Compute gap_ms since the last output
        nonlocal last_emit_ns
        t = now_ns()
        gap_ms = ns_to_ms_round_up(t - last_emit_ns)

        # If gap_ms >= delay_threshold, append <delay=gap_ms>
        if gap_ms >= args.delay_threshold:
            script_parts.append(f"<delay={gap_ms}>")

        # Update last_emit_ns to the current time
        last_emit_ns = t

    def emit(token: str):
        """Append a token, inserting <delay=...> if needed."""

        # Append the actual token
        emit_delay_if_needed()
        script_parts.append(token)

    def ordered_mods_snapshot():
        """Return modifiers in a fixed order for nicer output."""

        # Return pressed_mods in a fixed order
        # e.g. ctrl + alt + a, not alt + ctrl + a
        order = ["ctrl", "alt", "gui", "shift"]
        return [m for m in order if m in pressed_mods]

    def base_name_for_nonmod(key):
        """
        Return the token base for a NON-mod key:
        - printable char,
        - named key,
        - or function key.
        Returns None if unsupported or hotkey.
        """
        # Printable (KeyCode)
        if isinstance(key, keyboard.KeyCode) and key.char is not None:
            ch = key.char

            # Intercept our hotkey so it never gets recorded
            if ch == "`":
                return None  # backtick toggles recording; not recorded

            # Normalize newline/tab if they arrive as chars
            if ch in ("\n", "\r"):
                return "enter"
            if ch == "\t":
                return "tab"

            # Letters: lower-case; SHIFT appears via modifier if held
            if len(ch) == 1 and ch.isalpha():
                return ch.lower()

            # Digits/symbols: as-is
            if len(ch) == 1 and ch.isprintable():
                return ch

            return None

        # ESC is a hotkey (stop/save); never recorded
        if key == keyboard.Key.esc:
            return None

        if key in NAMED_KEYS:
            return NAMED_KEYS[key]
        if key in FUNCTION_KEYS:
            return FUNCTION_KEYS[key]

        return None

    def chord_str(mods, base=None):
        """Format 'ctrl + alt + a' or just 'a'."""

        # If both mods and base are given -> e.g. "ctrl + shift + a", etc.
        # If only mods -> e.g. "ctrl + alt"
        # If only base -> e.g. "a"

        if base is None:
            if mods:
                return " + ".join(mods)
            return ""
        if mods:
            return " + ".join(mods + [base])
        return base

    # ---------- Listeners ----------

    # Runs every time any key is pressed
    def on_press(key):
        nonlocal recording, last_emit_ns

        # Hotkeys first, so they don't get recorded
        # Pause/Resume on BACKTICK (`)
        if isinstance(key, keyboard.KeyCode) and key.char == "`":
            recording = not recording

            # When pausing, clear any partial state to avoid weird resumes
            if not recording:
                pending.clear()
                pressed_mods.clear()

            print("Recording:", recording)
            return

        # Stop and Save on ESC
        if key == keyboard.Key.esc:
            text = "".join(script_parts)
            with open(args.outfile, "w", encoding="utf-8") as f:
                f.write(text)

            print("\n--- Script ---\n")
            print(text)
            print("\nSaved to:", args.outfile)

            return False

        # Is this a modifier key?
        if key in MOD_MAP and MOD_MAP[key]:
            mod_name = MOD_MAP[key]
            # Update logical state
            pressed_mods.add(mod_name)
            if recording:
                emit(f"<down:{mod_name}>")
            return

        # If not currently recording, ignore the rest
        if not recording:
            return

        # Non-mod key: start tracking if it's not already pending
        if key not in pending:
            base = base_name_for_nonmod(key)
            if base is None:
                return  # unsupported or hotkey
            
            mods_snapshot = ordered_mods_snapshot()
            pending[key] = {"base": base}

            inside = chord_str(mods_snapshot, base)
            emit(f"<down:{inside}>")

    def on_release(key):
        # Modifier release?
        if key in MOD_MAP and MOD_MAP[key]:
            mod_name = MOD_MAP[key]
            if mod_name in pressed_mods:
                pressed_mods.discard(mod_name)
            if recording:
                emit(f"<up:{mod_name}>")
            return

        if not recording:
            return

        # Non-mod release: look up its base name
        rec = pending.pop(key, None)
        if rec is None:
            return

        base = rec["base"]
        emit(f"<up:{base}>")

    # ---------- Main loop ----------

    print("PolyCast5 Keystroke Recorder (down/up + delay, multi-key capable)")
    print("  `   = pause/resume recording")
    print("  Esc = stop & save (writes", args.outfile, ")")
    print("Recording:", recording)

    # Creates a keyboard.Listener with the on_press and on_release callbacks
    with keyboard.Listener(on_press=on_press, on_release=on_release) as listener:
        listener.join()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nExiting.")
