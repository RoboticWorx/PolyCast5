/**
 * @file ai_analysis_portal_html.c
 * @brief Embedded single-page HTML application for the AI packet analysis portal.
 *
 * Overview
 * --------
 * This file defines AI_ANALYSIS_PORTAL_HTML, a C string literal containing
 * a complete, self-contained HTML page that is served verbatim by the
 * ai_analysis_portal HTTP server (ai_analysis_portal.c) in response to
 * GET /.  No files are read from flash at runtime; the HTML is baked into
 * the firmware image at compile time.
 *
 * Data flow
 * ---------
 *  1. The PolyCast5 Wi-Fi task captures raw 802.11 frames (MGMT, CTRL, DATA,
 *     MISC, DATA_MPDU, DATA_AMPDU) and forwards them to the AI task.
 *  2. The AI task calls ai_utils_send_command_xai() with the packet analysis
 *     system prompt and the captured frame summary, receiving a Markdown
 *     response from the Grok (xAI) API.
 *  3. ai_analysis_portal_set_result() stores the Markdown text in the server's
 *     internal buffer (s_result, up to AI_RESPONSE_MAX_LEN bytes in PSRAM).
 *  4. The user connects to the open SoftAP "PolyCast5-AI-Analysis" and
 *     navigates to http://192.168.4.1.
 *  5. The HTTP server returns this HTML page for GET /.
 *  6. The page's load() function issues a GET /api/result request.
 *  7. The server's result_get() handler returns JSON:
 *       { "has_result": true, "md": "<markdown text>" }
 *  8. The client-side renderMd() function converts the Markdown to safe HTML
 *     and injects it into the #out div.
 *
 * Page structure
 * --------------
 *  - <head>: UTF-8 charset, responsive viewport, page title, CSS.
 *  - <body>: Static heading and three informational notes, followed by a
 *    <div id="out"> that initially shows "Loading..." and is replaced with
 *    the rendered analysis once the fetch completes.
 *  - <script>: Inline JavaScript (no external dependencies) for HTML escaping,
 *    Markdown rendering, and data fetching.
 *
 * Client-side Markdown renderer
 * ------------------------------
 * Because the Grok API returns Markdown, the page ships a lightweight renderer
 * that supports:
 *  - Fenced code blocks (``` ... ```) → <pre><code>
 *  - GFM-style pipe tables (header row + separator row + body rows) → <table>
 *  - Unordered lists (- item or * item) → <ul><li>
 *  - ATX headings (#, ##, ###) → <h1>/<h2>/<h3>
 *  - Inline bold (**text**), italic (*text*), and code (`text`)
 *  - Plain text wrapped in <div>; blank lines become <br>
 *
 * Security
 * --------
 * All Markdown text fetched from the server is passed through escHtml()
 * before any pattern matching is applied, ensuring that any HTML characters
 * in the AI response are neutralised before being injected into the DOM.
 * The /api/result endpoint is only reachable from the local SoftAP network;
 * the SoftAP is open but isolated to on-device IP space.
 */
const char *AI_ANALYSIS_PORTAL_HTML =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"  <head>\n"
"    <meta charset=\"utf-8\" />\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n"
"    <title>PolyCast5 AI Packet Analysis</title>\n"

// -----------------------------------------------------------------------
// CSS: Design tokens
//   All colours, spacing, and border-radius values are defined here as
//   CSS custom properties so every rule below references a single source
//   of truth.  The palette uses a deep navy background (--bg) with a
//   bright blue accent (--accent) to give a retro terminal aesthetic.
// -----------------------------------------------------------------------
"    <style>\n"
"      :root {\n"
"        --bg: #060b18;\n"
"        --bg-panel: #0c1528;\n"
"        --border: #1a3068;\n"
"        --accent: #5ba3f5;\n"
"        --accent-dim: #0047ab;\n"
"        --accent-glow: rgba(0,71,171,0.25);\n"
"        --text: #d6dff0;\n"
"        --text-bright: #f0f4fa;\n"
"        --page-padding: 20px;\n"
"        --radius: 4px;\n"
"      }\n"

"      *, *::before, *::after { box-sizing: border-box; }\n"

// CSS: Page body
//   Monospace font preserves the terminal feel.  The page is centred with a
//   900 px max-width so packet tables (which can be wide) have room to breathe.
//   position:relative is required so the pseudo-element overlay below is
//   positioned relative to the viewport, not the content flow.
"      body {\n"
"        font-family: 'Courier New', Courier, monospace;\n"
"        margin: 0;\n"
"        padding: var(--page-padding);\n"
"        background: var(--bg);\n"
"        color: var(--text);\n"
"        line-height: 1.45;\n"
"        max-width: 900px;\n"
"        margin: 0 auto;\n"
"        position: relative;\n"
"      }\n"

// CSS: CRT scanline overlay
//   A fixed ::after pseudo-element covers the entire viewport with a
//   repeating horizontal gradient that mimics the scanline pattern of an old
//   CRT monitor.  pointer-events:none ensures it never blocks clicks, and
//   z-index:9999 places it above all other content (purely cosmetic).
"      body::after {\n"
"        content: '';\n"
"        position: fixed;\n"
"        inset: 0;\n"
"        background: repeating-linear-gradient(\n"
"          0deg,\n"
"          transparent,\n"
"          transparent 2px,\n"
"          rgba(0,0,0,0.08) 2px,\n"
"          rgba(0,0,0,0.08) 4px\n"
"        );\n"
"        pointer-events: none;\n"
"        z-index: 9999;\n"
"      }\n"

// CSS: Section heading
//   h2 is the only top-level heading used in the static HTML.  The glow
//   text-shadow, uppercase transform, and wide letter-spacing reinforce the
//   terminal/cyberpunk aesthetic.
"      h2 {\n"
"        color: var(--accent);\n"
"        text-transform: uppercase;\n"
"        letter-spacing: 3px;\n"
"        font-size: 1.3em;\n"
"        border-bottom: 2px solid var(--accent-dim);\n"
"        padding-bottom: 10px;\n"
"        text-shadow: 0 0 10px rgba(91,163,245,0.4);\n"
"      }\n"

"      p {\n"
"        font-size: 0.85em;\n"
"        margin: 8px 0;\n"
"      }\n"

"      .small-note {\n"
"        opacity: 0.75;\n"
"        font-size: 0.8em;\n"
"      }\n"

// CSS: Output panel (#out)
//   This <div> is the only dynamic element on the page.  It starts with the
//   text "Loading..." and is replaced with rendered HTML once load() resolves.
//   overflow:auto adds scrollbars if the AI response is taller than the
//   viewport.  The nested rules below (#out pre, #out code, etc.) style the
//   HTML elements that renderMd() generates from the Markdown.
"      #out {\n"
"        background: var(--bg);\n"
"        color: var(--text-bright);\n"
"        padding: 16px;\n"
"        border-radius: var(--radius);\n"
"        border: 1px solid var(--border);\n"
"        overflow: auto;\n"
"        margin-top: 16px;\n"
"        font-size: 0.9em;\n"
"        line-height: 1.5;\n"
"      }\n"

"      #out pre {\n"
"        background: var(--bg);\n"
"        color: var(--text-bright);\n"
"        padding: 12px;\n"
"        border: 1px solid var(--border);\n"
"        border-radius: var(--radius);\n"
"        overflow: auto;\n"
"      }\n"

"      #out code {\n"
"        background: var(--bg);\n"
"        color: var(--accent);\n"
"        padding: 2px 5px;\n"
"        border-radius: 3px;\n"
"        font-family: inherit;\n"
"      }\n"

"      #out pre code {\n"
"        background: none;\n"
"        color: var(--text-bright);\n"
"        padding: 0;\n"
"      }\n"

"      #out a {\n"
"        color: var(--accent);\n"
"        text-decoration: none;\n"
"        border-bottom: 1px dashed var(--accent-dim);\n"
"      }\n"

"      #out h1, #out h2, #out h3 {\n"
"        color: var(--accent);\n"
"        margin: 16px 0 8px;\n"
"        text-transform: none;\n"
"        letter-spacing: normal;\n"
"        border: none;\n"
"        padding: 0;\n"
"        text-shadow: none;\n"
"      }\n"

"      #out h1 { font-size: 1.2em; }\n"
"      #out h2 { font-size: 1.1em; }\n"
"      #out h3 { font-size: 1em; }\n"

"      #out ul {\n"
"        margin: 8px 0 8px 20px;\n"
"      }\n"

"      #out table {\n"
"        border-collapse: collapse;\n"
"        width: 100%;\n"
"        margin: 12px 0;\n"
"      }\n"

"      #out th, #out td {\n"
"        border: 1px solid var(--border);\n"
"        padding: 6px 10px;\n"
"        vertical-align: top;\n"
"      }\n"

"      #out th {\n"
"        background: var(--bg);\n"
"        color: var(--accent);\n"
"        font-weight: 600;\n"
"      }\n"

// CSS: Selection and scrollbar
//   Text selection uses the accent colour for a consistent highlight.
//   The Webkit scrollbar overrides give the thin, dark scrollbar track seen
//   in Chromium-based browsers.  Firefox uses its own scrollbar styling.
"      ::selection {\n"
"        background: var(--accent);\n"
"        color: var(--bg);\n"
"      }\n"

"      ::-webkit-scrollbar { width: 6px; height: 6px; }\n"
"      ::-webkit-scrollbar-track { background: var(--bg); }\n"
"      ::-webkit-scrollbar-thumb { background: var(--accent-dim); border-radius: 3px; }\n"
"    </style>\n"
"  </head>\n"

// -----------------------------------------------------------------------
// HTML body: static UI
//   The visible page consists of:
//     h2   – page title
//     p.small-note (×3) – brief instructions / disclaimer
//     #out – placeholder replaced with rendered analysis by load()
//
//   The three informational paragraphs remind the user that results are
//   AI-generated (and may be inaccurate) and list the 802.11 frame classes
//   that the packet sniffer can capture.
// -----------------------------------------------------------------------
"  <body>\n"
"    <h2>PolyCast5 AI Packet Analysis</h2>\n"
"    <p class=\"small-note\">This page shows an AI analysis of the 802.11 packets captured.</p>\n"
"    <p class=\"small-note\">\n"
"      For your reference, packets captured can be of type:\n"
"      MGMT, CTRL, DATA, MISC, DATA_MPDU, or DATA_AMPDU.\n"
"    </p>\n"
"    <p class=\"small-note\">Please verify important information.</p>\n"
"    <div id=\"out\">Loading...</div>\n"

// -----------------------------------------------------------------------
// JavaScript: inline, no external dependencies
//
//   The script is divided into five logical sections:
//     1. escHtml()         – XSS guard for raw AI text
//     2. fmtInline()       – inline Markdown spans (bold, italic, code)
//     3. Table helpers     – isTableSep / splitTableRow / looksLikeTableRow
//     4. renderMd()        – full block-level Markdown → HTML renderer
//     5. load()            – async fetch from /api/result + DOM update
// -----------------------------------------------------------------------
"    <script>\n"
"      // -------------------------------\n"
"      // HTML escaping\n"
"      // -------------------------------\n"
// escHtml(s) – XSS guard
//   Escapes the five characters that are significant in HTML/XML:
//     &  →  &amp;   (must be first to avoid double-escaping)
//     <  →  &lt;
//     >  →  &gt;
//     "  →  &quot;
//     '  →  &#39;
//   Called on every piece of Markdown text before pattern matching so that
//   HTML markup injected by the AI response is rendered as visible text
//   rather than parsed as DOM nodes.
"      function escHtml(s) {\n"
"        return (s || '')\n"
"          .replace(/&/g, '&amp;')\n"
"          .replace(/</g, '&lt;')\n"
"          .replace(/>/g, '&gt;')\n"
"          .replace(/\"/g, '&quot;')\n"
"          .replace(/'/g, '&#39;');\n"
"      }\n"

"      // -------------------------------\n"
"      // Inline markdown formatting\n"
"      // -------------------------------\n"
// fmtInline(s) – inline span renderer
//   Applies three inline Markdown patterns using non-greedy regexes:
//     **text**  →  <strong>text</strong>   (bold)
//     *text*    →  <em>text</em>           (italic)
//     `text`    →  <code>text</code>       (inline code)
//   Called on individual lines and table cells *after* escHtml(), so the
//   input already has HTML entities.  The regexes therefore target literal
//   * and ` characters, not their entity equivalents.
"      function fmtInline(s) {\n"
"        return s\n"
"          .replace(/\\*\\*(.+?)\\*\\*/g, '<strong>$1</strong>')\n"
"          .replace(/\\*(.+?)\\*/g, '<em>$1</em>')\n"
"          .replace(/`([^`]+?)`/g, '<code>$1</code>');\n"
"      }\n"

"      // -------------------------------\n"
"      // Table helpers\n"
"      // -------------------------------\n"
// isTableSep(line) – GFM separator row detector
//   Returns true when a line looks like the dashes-only separator row that
//   GitHub Flavored Markdown places between the header and body rows, e.g.:
//     | --- | :---: | ---: |
//   The regex accepts optional leading/trailing pipes, optional colon
//   alignment markers, and requires at least two cell columns (one | between
//   them).
//
// splitTableRow(line) – pipe-delimited cell extractor
//   Strips optional leading and trailing | characters, then splits on |.
//   Each cell is trimmed of surrounding whitespace.
//   Example: "| Addr | Port |" → ["Addr", "Port"]
//
// looksLikeTableRow(line) – quick table row heuristic
//   Returns true if the line contains at least one | and has non-pipe
//   content, distinguishing a data row from an accidental divider or
//   empty line.
"      function isTableSep(line) {\n"
"        var t = (line || '').trim();\n"
"        if (!t) return false;\n"
"        return /^\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)+\\|?$/.test(t);\n"
"      }\n"

"      function splitTableRow(line) {\n"
"        var t = (line || '').trim();\n"
"        if (t.startsWith('|')) t = t.slice(1);\n"
"        if (t.endsWith('|')) t = t.slice(0, -1);\n"
"        return t.split('|').map(function(c) { return c.trim(); });\n"
"      }\n"

"      function looksLikeTableRow(line) {\n"
"        var t = (line || '').trim();\n"
"        return t.includes('|') && t.replace(/\\|/g, '').trim().length > 0;\n"
"      }\n"

"      // -------------------------------\n"
"      // Markdown renderer\n"
"      // -------------------------------\n"
// renderMd(md) – Markdown-to-HTML converter
//
//   Step 1 – Normalise line endings: \r\n → \n.
//
//   Step 2 – Split on ``` to separate fenced code blocks from regular
//   Markdown.  After splitting, odd-indexed parts are inside a fence and
//   even-indexed parts are outside.  Odd parts are HTML-escaped and wrapped
//   in <pre><code>...</code></pre> unchanged.
//
//   Step 3 – For each even (non-code) part, HTML-escape the entire segment,
//   then walk line by line applying block-level rules in priority order:
//
//     a) GFM tables
//        Triggered when the current line looks like a table row AND the very
//        next line is a separator row.  The header cells become <th> elements.
//        The separator line is consumed (li++).  Subsequent lines are
//        consumed as <tr><td> rows until a non-table line or blank is found.
//
//     b) Unordered lists
//        Lines starting with optional whitespace then - or * followed by a
//        space open a <ul> context (inList=true).  The bullet prefix is
//        stripped and the remainder becomes an <li>.  The list is closed
//        (<ul>) when a non-list line is encountered.
//
//     c) ATX headings (#, ##, ###)
//        Lines beginning with one to three # characters followed by a space
//        become <h1>, <h2>, or <h3>.  Heading text is not processed through
//        fmtInline() to avoid false matches inside heading content that has
//        already been HTML-escaped.
//
//     d) Plain text / blank lines
//        After inline formatting is applied via fmtInline(), blank lines
//        become <br> and all other lines become <div>line</div>, preserving
//        the original line structure without introducing implicit paragraph
//        merging.
//
//   Step 4 – If a list is still open at the end of a segment, close it.
//
//   Returns the concatenated HTML string for injection into #out.innerHTML.
"      function renderMd(md) {\n"
"        md = (md || '').replace(/\\r\\n/g, '\\n');\n"
"        var parts = md.split('```');\n"
"        var out = '';\n"

"        for (var i = 0; i < parts.length; i++) {\n"
"          // Odd parts are fenced code blocks\n"
"          if (i % 2 === 1) {\n"
"            out += '<pre><code>' + escHtml(parts[i]) + '</code></pre>';\n"
"            continue;\n"
"          }\n"

"          // Even parts are regular markdown\n"
"          var s = escHtml(parts[i]);\n"
"          var lines = s.split('\\n');\n"
"          var inList = false;\n"

"          for (var li = 0; li < lines.length; li++) {\n"
"            var line = lines[li];\n"

"            // Table detection: current line has pipes and next line is a separator\n"
"            if (looksLikeTableRow(line) && (li + 1 < lines.length) && isTableSep(lines[li + 1])) {\n"
"              if (inList) { out += '</ul>'; inList = false; }\n"
"              var head = splitTableRow(line);\n"
"              li++;\n"
"              out += '<table><thead><tr>';\n"

"              for (var c = 0; c < head.length; c++) {\n"
"                out += '<th>' + fmtInline(head[c]) + '</th>';\n"
"              }\n"
"              out += '</tr></thead><tbody>';\n"

"              for (li = li + 1; li < lines.length; li++) {\n"
"                var rline = lines[li];\n"
"                if (!looksLikeTableRow(rline) || rline.trim() === '') { li--; break; }\n"
"                var row = splitTableRow(rline);\n"
"                out += '<tr>';\n"

"                for (var rc = 0; rc < row.length; rc++) {\n"
"                  out += '<td>' + fmtInline(row[rc]) + '</td>';\n"
"                }\n"
"                out += '</tr>';\n"
"              }\n"

"              out += '</tbody></table>';\n"
"              continue;\n"
"            }\n"

"            // List items\n"
"            if (/^\\s*[-*]\\s+/.test(line)) {\n"
"              if (!inList) { out += '<ul>'; inList = true; }\n"
"              line = line.replace(/^\\s*[-*]\\s+/, '');\n"
"            } else {\n"
"              if (inList) { out += '</ul>'; inList = false; }\n"
"            }\n"

"            if (inList) {\n"
"              out += '<li>' + fmtInline(line) + '</li>';\n"
"              continue;\n"
"            }\n"

"            // Headings\n"
"            if (/^###\\s+/.test(line)) { out += '<h3>' + line.replace(/^###\\s+/, '') + '</h3>'; continue; }\n"
"            if (/^##\\s+/.test(line)) { out += '<h2>' + line.replace(/^##\\s+/, '') + '</h2>'; continue; }\n"
"            if (/^#\\s+/.test(line)) { out += '<h1>' + line.replace(/^#\\s+/, '') + '</h1>'; continue; }\n"

"            // Regular text or blank line\n"
"            line = fmtInline(line);\n"
"            if (line.trim() === '') { out += '<br>'; }\n"
"            else { out += '<div>' + line + '</div>'; }\n"
"          }\n"

"          if (inList) { out += '</ul>'; }\n"
"        }\n"

"        return out;\n"
"      }\n"

"      // -------------------------------\n"
"      // Fetch and render\n"
"      // -------------------------------\n"
// load() – async entry point, called once on page load
//
//   1. Fetches GET /api/result (same origin, the ESP HTTP server).
//   2. On a non-OK HTTP status, displays the status code as plain text.
//   3. Parses the JSON body:
//        { "has_result": <bool>, "md": "<markdown string>" }
//   4. If has_result is false, displays "No result available." as text.
//   5. Otherwise passes j.md through renderMd() and injects the resulting
//      HTML into #out.innerHTML.
//   6. Any network or parse error falls through to the catch block and
//      displays "Load failed" as plain text.
"      async function load() {\n"
"        var el = document.getElementById('out');\n"

"        try {\n"
"          var r = await fetch('/api/result');\n"
"          if (!r.ok) { el.textContent = 'HTTP ' + r.status; return; }\n"

"          var j = await r.json();\n"
"          if (!j.has_result) { el.textContent = 'No result available.'; return; }\n"

"          el.innerHTML = renderMd(j.md || '');\n"
"        } catch (e) {\n"
"          el.textContent = 'Load failed';\n"
"        }\n"
"      }\n"

"      load();\n"
"    </script>\n"
"  </body>\n"
"</html>\n"
;
