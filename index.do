export import class JsError from "./native_js.hpp" as doof_js::JsError {
  kind: string
  message: string
  stack: string | null
}

export import class JsEngine from "./native_js.hpp" as doof_js::JsEngine {
  static create(
    memoryLimitBytes: long = 67108864L,
    maxStackBytes: long = 1048576L,
    timeoutMillis: int = 1000,
  ): JsEngine

  exec(source: string): Result<void, JsError>
  eval(source: string): Result<JsonValue, JsError>
  callJson(functionName: string, args: JsonValue[]): Result<JsonValue, JsError>
}
