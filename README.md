# std/js

Persistent JSON-oriented JavaScript evaluation backed by QuickJS-NG.

```do
import { JsEngine } from "std/js"

engine := JsEngine()
try engine.exec("function add(a, b) { return a + b }")
try answer := engine.callJson("add", [20, 22])
```

`JsEngine` keeps one script context alive across calls. Use `exec` for loader-style scripts that only need side effects, and `eval` when the script should return a JSON-compatible value. JavaScript values that cross the boundary must be JSON; values such as `undefined`, functions, symbols, `bigint`, cyclic structures, and non-finite numbers return `JsError` from `eval` instead.

Doof callbacks can be registered as synchronous global JavaScript functions:

```do
try engine.bindJson("addOne", (args: JsonValue[]): Result<JsonValue, string> => {
  value := args[0] as int else { return Failure("expected int argument") }
  return Success(value + 1)
})
try answer := engine.eval("addOne(41)")
```

The default constructor profile is bounded for ordinary embedded scripting:

- 64 MiB memory limit
- 1 MiB stack limit
- 1000 ms timeout

Doof automatically acquires the pinned QuickJS-NG source tree into `vendor/quickjs` during build/test, so builds do not depend on a host JavaScript engine install.

## Documentation

- [Guide and API reference](docs/API.md) explains engine limits, JSON boundary rules, errors, and the exported QuickJS-backed API.
- Tests can be run with `doof test js`.
