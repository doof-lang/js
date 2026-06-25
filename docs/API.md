# std/js Guide

`std/js` embeds a persistent QuickJS-NG context for JSON-oriented JavaScript
evaluation. It is useful for plugin-style scripting, user expressions, migration
helpers, or compatibility code where Doof owns the host process and JavaScript is
an embedded guest.

Doof automatically acquires the pinned QuickJS-NG source tree into
`vendor/quickjs` during build/test, so callers do not need a host JavaScript
runtime installed.

## Quick Start

```doof
import { JsEngine } from "std/js"

engine := JsEngine()
try! engine.exec("function add(a, b) { return a + b }")
answer := try! engine.callJson("add", [20, 22])
```

`JsEngine` keeps one JavaScript context alive across calls. Values defined with
`exec` remain available to later `eval` and `callJson` calls on the same engine.

## Engine Limits

The default constructor is bounded for ordinary embedded scripting:

- `memoryLimitBytes: 67108864L` (64 MiB)
- `maxStackBytes: 1048576L` (1 MiB)
- `timeoutMillis: 1000`

Pass explicit constructor values when running larger trusted scripts or when you
need tighter limits for untrusted input.

## JSON Boundary

Values crossing from JavaScript to Doof must be JSON-compatible. `eval` and
`callJson` can return strings, numbers, booleans, null, arrays, and objects that
fit Doof's `JsonValue` representation.

JavaScript values such as `undefined`, functions, symbols, `bigint`, cyclic
structures, and non-finite numbers fail with `JsError`.

Arguments passed to `callJson` are `JsonValue[]`, so the same JSON boundary
applies in the other direction.

## Errors

All execution methods return `Result<..., JsError>`. `JsError.kind` identifies
the broad failure category, `message` contains the human-readable failure, and
`stack` is present when QuickJS can provide a JavaScript stack trace.

Use `try!` for setup scripts that should abort the caller on failure, and handle
`Failure` explicitly when evaluating user input.

## API

### `JsError`

```doof
export import class JsError
```

Fields:

- `kind: string`
- `message: string`
- `stack: string | null`

Defined in [index.do](../index.do).

### `JsEngine`

```doof
export import class JsEngine
```

Constructor:

```doof
static constructor(
  memoryLimitBytes: long = 67108864L,
  maxStackBytes: long = 1048576L,
  timeoutMillis: int = 1000,
): JsEngine
```

Methods:

- `exec(source: string): Result<void, JsError>` runs source for side effects.
- `eval(source: string): Result<JsonValue, JsError>` evaluates source and returns a JSON-compatible value.
- `callJson(functionName: string, args: JsonValue[]): Result<JsonValue, JsError>` calls a global JavaScript function with JSON arguments.

Defined in [index.do](../index.do).
