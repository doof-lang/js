import { JsEngine, JsError, JsJsonHandler } from "../index"
import { formatJsonValue } from "std/json"

function requireEngine(): JsEngine {
  return JsEngine()
}

function requireEval(engine: JsEngine, source: string): JsonValue {
  let value: JsonValue = null
  let found = false

  case engine.eval(source) {
    s: Success -> {
      value = s.value
      found = true
    }
    f: Failure -> assert(false, "expected eval success: ${f.error.kind}: ${f.error.message}")
  }

  assert(found, "expected eval helper to produce a value")
  return value
}

function requireExec(engine: JsEngine, source: string): void {
  case engine.exec(source) {
    s: Success -> {}
    f: Failure -> assert(false, "expected exec success: ${f.error.kind}: ${f.error.message}")
  }
}

function requireCall(engine: JsEngine, name: string, args: JsonValue[]): JsonValue {
  let value: JsonValue = null
  let found = false

  case engine.callJson(name, args) {
    s: Success -> {
      value = s.value
      found = true
    }
    f: Failure -> assert(false, "expected call success: ${f.error.kind}: ${f.error.message}")
  }

  assert(found, "expected call helper to produce a value")
  return value
}

function requireEvalFailure(engine: JsEngine, source: string): JsError {
  let error: JsError | null = null

  case engine.eval(source) {
    s: Success -> assert(false, "expected eval failure")
    f: Failure -> { error = f.error }
  }

  assert(error != null, "expected eval failure helper to produce an error")
  return error!
}

function requireExecFailure(engine: JsEngine, source: string): JsError {
  let error: JsError | null = null

  case engine.exec(source) {
    s: Success -> assert(false, "expected exec failure")
    f: Failure -> { error = f.error }
  }

  assert(error != null, "expected exec failure helper to produce an error")
  return error!
}

function requireCallFailure(engine: JsEngine, name: string, args: JsonValue[] = []): JsError {
  let error: JsError | null = null

  case engine.callJson(name, args) {
    s: Success -> assert(false, "expected call failure")
    f: Failure -> { error = f.error }
  }

  assert(error != null, "expected call failure helper to produce an error")
  return error!
}

function requireBind(engine: JsEngine, name: string, handler: JsJsonHandler): void {
  case engine.bindJson(name, handler) {
    s: Success -> {}
    f: Failure -> assert(false, "expected bind success: ${f.error.kind}: ${f.error.message}")
  }
}

function requireBindFailure(engine: JsEngine, name: string, handler: JsJsonHandler): JsError {
  let error: JsError | null = null

  case engine.bindJson(name, handler) {
    s: Success -> assert(false, "expected bind failure")
    f: Failure -> { error = f.error }
  }

  assert(error != null, "expected bind failure helper to produce an error")
  return error!
}

export function testEvalRoundTripsJsonValues(): void {
  engine := requireEngine()
  value := requireEval(engine, "({ ok: true, items: [1, 2, 'three'], nested: { value: null } })")
  assert(
    formatJsonValue(value) == "{\"ok\":true,\"items\":[1,2,\"three\"],\"nested\":{\"value\":null}}",
    "expected JSON values to round-trip"
  )
}

export function testDefinitionsPersistAcrossCalls(): void {
  engine := requireEngine()
  requireExec(engine, "function add(a, b) { return a + b }")
  answer := requireCall(engine, "add", [20, 22])
  assert(formatJsonValue(answer) == "42", "expected globals to persist across engine calls")
}

export function testBoundDoofHandlerIsCallableFromJavaScript(): void {
  engine := requireEngine()
  requireBind(engine, "addOne", (args: JsonValue[]): Result<JsonValue, string> => {
    value := args[0] as int else { return Failure("expected int argument") }
    return Success(value + 1)
  })

  answer := requireEval(engine, "addOne(41)")
  assert(formatJsonValue(answer) == "42", "expected JS to call registered Doof handler")
}

export function testBoundHandlerReceivesMultipleJsonArguments(): void {
  engine := requireEngine()
  let received = ""
  requireBind(engine, "capture", (args: JsonValue[]): Result<JsonValue, string> => {
    received = formatJsonValue(args)
    return Success(args.length)
  })

  answer := requireEval(engine, "capture('a', 2, { ok: true })")
  assert(formatJsonValue(answer) == "3", "expected handler to return argument count")
  assert(received == "[\"a\",2,{\"ok\":true}]", "expected handler to receive JS arguments as JsonValue[]")
}

export function testBoundHandlerWorksThroughPersistedFunctionsAndCallJson(): void {
  engine := requireEngine()
  requireBind(engine, "double", (args: JsonValue[]): Result<JsonValue, string> => {
    value := args[0] as int else { return Failure("expected int argument") }
    return Success(value * 2)
  })
  requireExec(engine, "function useDouble(value) { return double(value) + 1 }")

  answer := requireCall(engine, "useDouble", [20])
  assert(formatJsonValue(answer) == "41", "expected callJson to reach Doof handler through JS")
}

export function testBoundHandlerReplacement(): void {
  engine := requireEngine()
  requireBind(engine, "value", (args: JsonValue[]): Result<JsonValue, string> => Success("old"))
  requireBind(engine, "value", (args: JsonValue[]): Result<JsonValue, string> => Success("new"))

  answer := requireEval(engine, "value()")
  assert(formatJsonValue(answer) == "\"new\"", "expected rebinding to replace the previous handler")
}

export function testBindingEmptyNameFails(): void {
  engine := requireEngine()
  error := requireBindFailure(engine, "", (args: JsonValue[]): Result<JsonValue, string> => Success(null))
  assert(error.kind == "bind", "expected empty binding names to use bind kind")
}

export function testBoundHandlerFailureSurfacesAsRuntimeError(): void {
  engine := requireEngine()
  requireBind(engine, "failDoof", (args: JsonValue[]): Result<JsonValue, string> => Failure("nope"))

  error := requireEvalFailure(engine, "failDoof()")
  assert(error.kind == "runtime", "expected handler failure to become a JS runtime failure")
  assert(error.message.contains("nope"), "expected handler failure message")
}

export function testBoundHandlerRejectsNonJsonArguments(): void {
  engine := requireEngine()
  requireBind(engine, "echo", (args: JsonValue[]): Result<JsonValue, string> => Success(args.length))

  error := requireEvalFailure(engine, "echo(undefined)")
  assert(error.kind == "runtime", "expected non-JSON JS arguments to throw during callback")
  assert(error.message.contains("JSON-compatible"), "expected JSON boundary message")
}

export function testExecAcceptsNonJsonCompletionValues(): void {
  engine := requireEngine()
  requireExec(engine, "undefined")
  requireExec(engine, "(() => 1)")
}

export function testSyntaxAndRuntimeErrorsAreStructured(): void {
  engine := requireEngine()

  syntax := requireEvalFailure(engine, "function (")
  assert(syntax.kind == "syntax", "expected syntax errors to use the syntax kind")

  runtime := requireEvalFailure(engine, "throw new Error('boom')")
  assert(runtime.kind == "runtime", "expected runtime errors to use the runtime kind")
  assert(runtime.message.contains("boom"), "expected runtime error messages")
  assert(runtime.stack != null, "expected runtime stack text when QuickJS provides it")
}

export function testExecErrorsAreStructured(): void {
  engine := requireEngine()

  syntax := requireExecFailure(engine, "function (")
  assert(syntax.kind == "syntax", "expected exec syntax errors to use the syntax kind")

  runtime := requireExecFailure(engine, "throw new Error('boom')")
  assert(runtime.kind == "runtime", "expected exec runtime errors to use the runtime kind")
}

export function testCallFailuresAreStructured(): void {
  engine := requireEngine()
  requireEval(engine, "const notAFunction = 1; function explode() { throw new Error('nope') }; null")

  missing := requireCallFailure(engine, "missing")
  assert(missing.kind == "call", "expected missing functions to use the call kind")

  notCallable := requireCallFailure(engine, "notAFunction")
  assert(notCallable.kind == "call", "expected non-functions to use the call kind")

  thrown := requireCallFailure(engine, "explode")
  assert(thrown.kind == "runtime", "expected function throws to use runtime kind")
}

export function testRejectsNonJsonResults(): void {
  engine := requireEngine()
  assert(requireEvalFailure(engine, "undefined").kind == "convert", "expected undefined to fail conversion")
  assert(requireEvalFailure(engine, "(() => 1)").kind == "convert", "expected functions to fail conversion")
  assert(requireEvalFailure(engine, "1n").kind == "convert", "expected bigint to fail conversion")
  assert(requireEvalFailure(engine, "NaN").kind == "convert", "expected non-finite numbers to fail conversion")
  assert(
    requireEvalFailure(engine, "(() => { const value = {}; value.self = value; return value })()").kind == "convert",
    "expected cyclic values to fail conversion"
  )
}

export function testExecutionLimitsReturnErrors(): void {
  timeoutEngine := JsEngine { timeoutMillis: 10 }
  timeout := requireEvalFailure(timeoutEngine, "while (true) {}")
  assert(timeout.kind == "runtime", "expected interrupted execution to surface as runtime failure")

  stackEngine := JsEngine { maxStackBytes: 32768L }
  stack := requireEvalFailure(stackEngine, "function recurse() { return recurse() }; recurse()")
  assert(stack.kind == "runtime", "expected stack exhaustion to surface as runtime failure")

  memoryEngine := JsEngine { memoryLimitBytes: 2097152L }
  memory := requireEvalFailure(memoryEngine, "new Array(1000000).fill('x')")
  assert(memory.kind == "runtime", "expected memory exhaustion to surface as runtime failure")
}
