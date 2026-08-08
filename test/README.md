# P4MiniShell Unit Tests

This directory contains unit tests for the P4MiniShell shell components.
Tests use the Unity test framework (included in ESP-IDF).

## Test Structure

```
test/
  CMakeLists.txt          # Test project build configuration
  main/
    CMakeLists.txt        # Test main component
    test_main.c           # Test runner entry point
  test_shell_parser.c     # Shell parser tests (split_args, trim, text_equals)
  test_shell_history.c    # Command history tests
  test_wifi_state.c       # Wi-Fi state machine tests
  test_ansi_format.c      # ANSI format string builder tests
```

## Running Tests

From the project root:

```powershell
$env:IDF_PATH = "C:\esp\v5.5.5\esp-idf"
. $env:IDF_PATH\export.ps1
cd test
idf.py build flash monitor
```

## Test Coverage

- **Shell parser**: tokenization, quote handling, edge cases
- **Command history**: store, recall, password masking, overflow
- **Wi-Fi state machine**: state transitions, mutex, watchdog logic
- **ANSI format**: color specifiers, escape sequence generation
