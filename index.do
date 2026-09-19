export import class JsError from "./native_js.hpp" as doof_js::JsError {
  kind: string
  message: string
  stack: string | none
}

export type JsJsonHandler = (args: SerialValue[]): Result<SerialValue, string>

export import class JsEngine from "./native_js.hpp" as doof_js::JsEngine {
  isolated static constructor(
    memoryLimitBytes: long = 67108864L,
    maxStackBytes: long = 1048576L,
    timeoutMillis: int = 1000,
  ): JsEngine

  isolated exec(source: string): Result<none, JsError>
  isolated eval(source: string): Result<SerialValue, JsError>
  isolated callJson(functionName: string, args: SerialValue[]): Result<SerialValue, JsError>
  isolated bindJson(name: string, handler: JsJsonHandler): Result<none, JsError>
}
