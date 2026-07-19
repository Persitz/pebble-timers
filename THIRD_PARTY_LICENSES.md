# Third-Party Licenses

pebble-timers itself is licensed under the Apache License, Version 2.0 (see `LICENSE`),
Copyright 2026 Herman Persitz (see `NOTICE`). This file details the third-party works
redistributed within this repository, and the one third-party work consumed as a separate,
independently licensed component.

Upstream copyright and license text below is reproduced as published by each project and is not
relabeled or modified.

---

## Unity

A unit testing framework for C.

- **Upstream:** https://github.com/ThrowTheSwitch/Unity
- **Vendored at:** `app/test/vendor/unity/`
- **License:** MIT
- **Copyright:** Copyright (c) 2007-25 Mike Karlesky, Mark VanderVoord, & Greg Williams

```
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

The SPDX identifier `MIT` is carried in the vendored source itself (`app/test/vendor/unity/unity.h`,
`unity.c`, `unity_internals.h`).

---

## FFF (Fake Function Framework)

A fake-function/mock generation library for C, used as the adapter-seam fake-function library in
this project's host unit tests.

- **Upstream:** https://github.com/meekrosoft/fff
- **Vendored at:** `app/test/vendor/fff.h`
- **License:** MIT
- **Copyright:** Copyright (c) 2010 Michael Long

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The full upstream notice is also preserved verbatim in the vendored file's own header comment
(`app/test/vendor/fff.h`).

---

## JetBrains Mono Bold

A monospaced typeface, redistributed as the app's display font.

- **Upstream:** https://github.com/JetBrains/JetBrainsMono
- **Redistributed at:** `app/resources/fonts/JetBrainsMono-Bold.ttf`
- **License:** SIL Open Font License, Version 1.1
- **Copyright:** Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)

The complete OFL 1.1 license text is preserved at `app/resources/fonts/OFL.txt` and is not
reproduced here; that file is redistributed alongside the font as required by the license.

---

## pebble-testpilot (emulator/test harness)

The reusable, app-agnostic QEMU-driving emulator/test infrastructure this project uses for
emulator-backed verification and screenshotting.

- **Upstream:** https://github.com/Persitz/pebble-testpilot
- **Consumed as:** a git submodule at `harness/`, pinned to a specific upstream commit
- **License:** Apache License, Version 2.0 (its own `LICENSE`/`NOTICE`, not this project's)

`harness/` is not redistributed source of this repository — it is fetched separately from its own
public remote when the submodule is checked out, and it carries its own license and notice files
at that path. See `harness/LICENSE` and `harness/NOTICE` after checkout.
