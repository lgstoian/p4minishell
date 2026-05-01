# DeepSeek V4 Coding Constraints

1. **Strict Output Format**
   - Produce ONLY a valid OpenCode edit block.
   - Format: ` ```language:path/relative/to/root\n...code...\n``` `
   - Zero text before, after, or outside the block. No explanations, no headings, no “Here is the edit”.
   - If you cannot deliver a single valid edit block, output nothing.

2. **Modular Code Structure**
   - NEVER place all logic in a single file (e.g., `main.c`).
   - Split into multiple `.c`/`.h` files by responsibility (e.g., `io.c`, `parser.c`, `config.c`).
   - Each file must have a clear, focused purpose.

3. **No Hardcoded Strings**
   - Define ALL user‑facing messages, paths, configuration keys, error strings, and any string that might change as named constants (e.g., `#define`, `const char*`, or `extern const`).
   - Avoid string literals directly in function calls; always use the defined identifier.
   - Externalise strings that may need localisation or adjustment.

4. **Additional Safeguards**
   - No unnecessary global variables; prefer passing state explicitly.
   - Every header must have include guards.
   - Mark internal functions `static`.
   - Code comments only inside the edit block; no external commentary.