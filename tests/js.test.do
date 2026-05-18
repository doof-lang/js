import { JsEngine, JsError } from "../index"
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
