export import class JsError from "./native_js.hpp" as doof_js::JsError {
  kind: string
  message: string
  stack: string | null
}

export type JsJsonHandler = (args: JsonValue[]): Result<JsonValue, string>

export import class JsEngine from "./native_js.hpp" as doof_js::JsEngine {
  isolated static constructor(
    memoryLimitBytes: long = 67108864L,
    maxStackBytes: long = 1048576L,
    timeoutMillis: int = 1000,
  ): JsEngine

  isolated exec(source: string): Result<void, JsError>
  isolated eval(source: string): Result<JsonValue, JsError>
  isolated callJson(functionName: string, args: JsonValue[]): Result<JsonValue, JsError>
  isolated bindJson(name: string, handler: JsJsonHandler): Result<void, JsError>
}
