# Contributing

Thanks for helping! Bug reports, ideas and pull requests are welcome.

## Before you start

- For anything bigger than a small fix, open an issue first so we can
  agree on the approach.
- Code and comments are in English.
- Make sure the firmware still builds with ESP-IDF v6.1 and, if you can,
  try it on an ESP32-C3:

  ```bash
  idf.py set-target esp32c3 && idf.py build
  ```

- Properties are part of the model: only ever append new ones, never
  change or reorder the existing ids. Update the table in the README.
- Changes to the protocol itself belong in
  [smart-core](https://github.com/siakinnik/smart-core).

## License and the CLA

The project is licensed under GPL-3.0-or-later. New source files start
with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) <year> <your name>
```

Before your first pull request can be merged you sign the
[Contributor License Agreement](CLA.md). You keep the copyright in your
work; the agreement lets the maintainer also license it on other terms
(dual licensing), while the GPL version stays open for everyone. The CLA
bot explains how to sign right in the pull request: it's one comment, and
it covers all your future contributions.
