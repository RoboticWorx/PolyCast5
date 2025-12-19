# Copilot instructions

## Code style (must follow)
Whenever you write or modify code in this repository, you MUST match the coding style of the surrounding project code in the files you are touching. This includes (but is not limited to):

- Indentation style and width (tabs vs spaces, alignment, continuation indent)
- Brace placement, spacing, and line wrapping
- Comment style and density (// vs /* */, inline vs block, tone/verbosity)
- Naming conventions (functions, types, macros, constants, variables, files)
- Existing patterns for error handling, logging, and assertions
- Include ordering, module boundaries, and how dependencies are referenced
- ESP-IDF conventions already used in the project (e.g., `ESP_LOGx`, `esp_err_t` flow)

If the local style is unclear, prefer:
1) the style already used in the same file, then
2) the style used in the closest related module/component folder.

Do not introduce new formatting rules or “personal preferences”. Do not reformat unrelated code.

## Scope and safety
- Make minimal, focused changes that solve the requested problem.
- Avoid adding new features unless explicitly asked.
- Keep changes consistent with ESP-IDF best practices already present in the repo.