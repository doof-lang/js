# std/js

Persistent JSON-oriented JavaScript evaluation backed by QuickJS-NG.

```do
import { JsEngine } from "std/js"

engine := JsEngine()
try engine.exec("function add(a, b) { return a + b }")
try answer := engine.callJson("add", [20, 22])
```

`JsEngine` keeps one script context alive across calls. Use `exec` for loader-style scripts that only need side effects, and `eval` when the script should return a JSON-compatible value. JavaScript values that cross the boundary must be JSON; values such as `undefined`, functions, symbols, `bigint`, cyclic structures, and non-finite numbers return `JsError` from `eval` instead.

The default constructor profile is bounded for ordinary embedded scripting:

- 64 MiB memory limit
- 1 MiB stack limit
- 1000 ms timeout

The package vendors QuickJS-NG sources under `vendor/quickjs` so builds do not depend on a host JavaScript engine install.
