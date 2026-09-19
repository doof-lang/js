#pragma once

#include "doof_runtime.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <quickjs.h>

namespace doof_js {

struct JsError {
    std::string kind;
    std::string message;
    std::optional<std::string> stack;

    JsError(std::string kind, std::string message, std::optional<std::string> stack = std::nullopt)
        : kind(std::move(kind)), message(std::move(message)), stack(std::move(stack)) {}
};

class JsEngine {
public:
    using JsonHandler = doof::callback<doof::Result<doof::SerialValue, std::string>(
        std::shared_ptr<std::vector<doof::SerialValue>>
    )>;

    static std::shared_ptr<JsEngine> constructor(
        int64_t memoryLimitBytes = 64LL * 1024LL * 1024LL,
        int64_t maxStackBytes = 1024LL * 1024LL,
        int32_t timeoutMillis = 1000
    ) {
        return std::shared_ptr<JsEngine>(new JsEngine(memoryLimitBytes, maxStackBytes, timeoutMillis));
    }

    ~JsEngine() {
        if (ctx_ != nullptr) {
            JS_FreeValue(ctx_, validator_);
            JS_FreeContext(ctx_);
        }
        if (runtime_ != nullptr) {
            JS_FreeRuntime(runtime_);
        }
    }

    doof::Result<doof::SerialValue, std::shared_ptr<JsError>> eval(const std::string& source) {
        beginExecution();
        JSValue value = JS_Eval(ctx_, source.c_str(), source.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);
        endExecution();

        if (JS_IsException(value)) {
            JS_FreeValue(ctx_, value);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError()};
        }

        auto converted = valueToJson(value);
        JS_FreeValue(ctx_, value);
        return converted;
    }

    doof::Result<void, std::shared_ptr<JsError>> exec(const std::string& source) {
        beginExecution();
        JSValue value = JS_Eval(ctx_, source.c_str(), source.size(), "<exec>", JS_EVAL_TYPE_GLOBAL);
        endExecution();

        if (JS_IsException(value)) {
            JS_FreeValue(ctx_, value);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError()};
        }

        JS_FreeValue(ctx_, value);
        return doof::Success<void>{};
    }

    doof::Result<doof::SerialValue, std::shared_ptr<JsError>> callJson(
        const std::string& functionName,
        const std::shared_ptr<std::vector<doof::SerialValue>>& args
    ) {
        JSValue global = JS_GetGlobalObject(ctx_);
        JSValue function = JS_GetPropertyStr(ctx_, global, functionName.c_str());
        if (JS_IsException(function)) {
            JS_FreeValue(ctx_, global);
            JS_FreeValue(ctx_, function);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError()};
        }
        if (!JS_IsFunction(ctx_, function)) {
            JS_FreeValue(ctx_, function);
            JS_FreeValue(ctx_, global);
            return failure("call", "Global '" + functionName + "' is not a function");
        }

        std::vector<JSValue> jsArgs;
        jsArgs.reserve(args ? args->size() : 0);
        if (args) {
            for (const auto& arg : *args) {
                auto converted = jsonToValue(arg);
                if (doof::is_failure(converted)) {
                    freeValues(jsArgs);
                    JS_FreeValue(ctx_, function);
                    JS_FreeValue(ctx_, global);
                    return doof::Failure<std::shared_ptr<JsError>>{doof::failure_error(converted)};
                }
                jsArgs.push_back(doof::success_value(converted));
            }
        }

        beginExecution();
        JSValue result = JS_Call(ctx_, function, global, static_cast<int>(jsArgs.size()), jsArgs.data());
        endExecution();
        freeValues(jsArgs);
        JS_FreeValue(ctx_, function);
        JS_FreeValue(ctx_, global);

        if (JS_IsException(result)) {
            JS_FreeValue(ctx_, result);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError()};
        }

        auto converted = valueToJson(result);
        JS_FreeValue(ctx_, result);
        return converted;
    }

    doof::Result<void, std::shared_ptr<JsError>> bindJson(
        const std::string& name,
        JsonHandler handler
    ) {
        if (name.empty()) {
            return doof::Failure<std::shared_ptr<JsError>>{makeError("bind", "JavaScript binding name must not be empty")};
        }

        handlers_.insert_or_assign(name, std::move(handler));

        JSValue global = JS_GetGlobalObject(ctx_);
        JSValue functionName = JS_NewStringLen(ctx_, name.data(), name.size());
        if (JS_IsException(functionName)) {
            JS_FreeValue(ctx_, global);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError("bind")};
        }

        JSValue data[1] = { functionName };
        JSValue function = JS_NewCFunctionData(ctx_, &JsEngine::boundJsonFunction, 0, 0, 1, data);
        JS_FreeValue(ctx_, functionName);
        if (JS_IsException(function)) {
            JS_FreeValue(ctx_, global);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError("bind")};
        }

        if (JS_SetPropertyStr(ctx_, global, name.c_str(), function) < 0) {
            JS_FreeValue(ctx_, global);
            return doof::Failure<std::shared_ptr<JsError>>{exceptionError("bind")};
        }

        JS_FreeValue(ctx_, global);
        return doof::Success<void>{};
    }

private:
    using JsonValueResult = doof::Result<doof::SerialValue, std::shared_ptr<JsError>>;
    using JsValueResult = doof::Result<JSValue, std::shared_ptr<JsError>>;

    JSRuntime* runtime_ = nullptr;
    JSContext* ctx_ = nullptr;
    JSValue validator_ = JS_UNDEFINED;
    int32_t timeoutMillis_ = 0;
    bool running_ = false;
    std::chrono::steady_clock::time_point deadline_ {};
    std::map<std::string, JsonHandler> handlers_;

    JsEngine(int64_t memoryLimitBytes, int64_t maxStackBytes, int32_t timeoutMillis)
        : timeoutMillis_(timeoutMillis) {
        if (memoryLimitBytes <= 0 || maxStackBytes <= 0 || timeoutMillis <= 0) {
            doof::panic("js engine limits must be positive");
        }

        runtime_ = JS_NewRuntime();
        if (runtime_ == nullptr) {
            doof::panic("failed to create QuickJS runtime");
        }
        JS_SetMemoryLimit(runtime_, static_cast<size_t>(memoryLimitBytes));
        JS_SetMaxStackSize(runtime_, static_cast<size_t>(maxStackBytes));
        JS_SetInterruptHandler(runtime_, &JsEngine::interruptHandler, this);

        ctx_ = JS_NewContext(runtime_);
        if (ctx_ == nullptr) {
            JS_FreeRuntime(runtime_);
            runtime_ = nullptr;
            doof::panic("failed to create QuickJS context");
        }
        JS_SetContextOpaque(ctx_, this);

        static constexpr const char* validatorSource = R"JS(
(value => {
  const seen = new Set();
  const visit = (current) => {
    if (current === null) return;
    const type = typeof current;
    if (type === "string" || type === "boolean") return;
    if (type === "number") {
      if (!Number.isFinite(current)) throw new TypeError("value is not JSON-compatible: non-finite number");
      return;
    }
    if (type !== "object") throw new TypeError("value is not JSON-compatible: " + type);
    if (seen.has(current)) throw new TypeError("value is not JSON-compatible: cyclic object graph");
    seen.add(current);
    if (Array.isArray(current)) {
      for (const item of current) visit(item);
    } else {
      const prototype = Object.getPrototypeOf(current);
      if (prototype !== Object.prototype && prototype !== null) {
        throw new TypeError("value is not JSON-compatible: non-plain object");
      }
      for (const key of Object.keys(current)) visit(current[key]);
    }
    seen.delete(current);
  };
  visit(value);
  return JSON.stringify(value);
})
)JS";

        validator_ = JS_Eval(ctx_, validatorSource, std::char_traits<char>::length(validatorSource), "<json-validator>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(validator_)) {
            auto error = exceptionError();
            JS_FreeValue(ctx_, validator_);
            validator_ = JS_UNDEFINED;
            doof::panic("failed to initialize JS JSON validator: " + error->message);
        }
    }

    static int interruptHandler(JSRuntime*, void* opaque) {
        auto* engine = static_cast<JsEngine*>(opaque);
        if (!engine->running_) {
            return 0;
        }
        return std::chrono::steady_clock::now() >= engine->deadline_ ? 1 : 0;
    }

    static JSValue boundJsonFunction(
        JSContext* ctx,
        JSValueConst,
        int argc,
        JSValueConst* argv,
        int,
        JSValue* funcData
    ) {
        auto* engine = static_cast<JsEngine*>(JS_GetContextOpaque(ctx));
        if (engine == nullptr) {
            return JS_ThrowInternalError(ctx, "missing Doof JavaScript engine");
        }

        const char* nameText = JS_ToCString(ctx, funcData[0]);
        if (nameText == nullptr) {
            return JS_EXCEPTION;
        }
        std::string name(nameText);
        JS_FreeCString(ctx, nameText);

        return engine->callBoundJson(name, argc, argv);
    }

    JSValue callBoundJson(const std::string& name, int argc, JSValueConst* argv) {
        auto handler = handlers_.find(name);
        if (handler == handlers_.end()) {
            return JS_ThrowReferenceError(ctx_, "No Doof binding registered for '%s'", name.c_str());
        }

        auto args = std::make_shared<std::vector<doof::SerialValue>>();
        args->reserve(static_cast<size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            auto converted = valueToJson(argv[index]);
            if (doof::is_failure(converted)) {
                const auto& error = doof::failure_error(converted);
                return JS_ThrowTypeError(ctx_, "%s", error->message.c_str());
            }
            args->push_back(doof::success_value(converted));
        }

        try {
            auto result = handler->second.call(args);
            if (doof::is_failure(result)) {
                return JS_ThrowPlainError(ctx_, "%s", doof::failure_error(result).c_str());
            }

            auto converted = jsonToValue(doof::success_value(result));
            if (doof::is_failure(converted)) {
                const auto& error = doof::failure_error(converted);
                return JS_ThrowTypeError(ctx_, "%s", error->message.c_str());
            }
            return doof::success_value(converted);
        } catch (const doof::Panic& e) {
            return JS_ThrowInternalError(ctx_, "%s", e.what());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx_, "%s", e.what());
        }
    }

    void beginExecution() {
        running_ = true;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis_);
    }

    void endExecution() {
        running_ = false;
    }

    JsonValueResult valueToJson(JSValueConst value) {
        JSValue argv[1] = { JS_DupValue(ctx_, value) };
        beginExecution();
        JSValue serialized = JS_Call(ctx_, validator_, JS_UNDEFINED, 1, argv);
        endExecution();
        JS_FreeValue(ctx_, argv[0]);

        if (JS_IsException(serialized)) {
            JS_FreeValue(ctx_, serialized);
            auto error = exceptionError();
            error->kind = "convert";
            return doof::Failure<std::shared_ptr<JsError>>{error};
        }
        if (!JS_IsString(serialized)) {
            JS_FreeValue(ctx_, serialized);
            return failure("convert", "value is not JSON-compatible");
        }

        JS_FreeValue(ctx_, serialized);
        return jsToJsonValue(value);
    }

    JsonValueResult jsToJsonValue(JSValueConst value) {
        if (JS_IsNull(value)) {
            return doof::Success<doof::SerialValue>{doof::SerialValue(nullptr)};
        }
        if (JS_IsBool(value)) {
            return doof::Success<doof::SerialValue>{doof::SerialValue(static_cast<bool>(JS_ToBool(ctx_, value)))};
        }
        if (JS_IsString(value)) {
            const char* text = JS_ToCString(ctx_, value);
            if (text == nullptr) {
                return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
            }
            std::string copied(text);
            JS_FreeCString(ctx_, text);
            return doof::Success<doof::SerialValue>{doof::SerialValue(std::move(copied))};
        }
        if (JS_IsNumber(value)) {
            double number = 0.0;
            if (JS_ToFloat64(ctx_, &number, value) < 0) {
                return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
            }
            if (!std::isfinite(number)) {
                return failure("convert", "value is not JSON-compatible: non-finite number");
            }
            if (std::trunc(number) == number) {
                if (number >= static_cast<double>(std::numeric_limits<int32_t>::min())
                    && number <= static_cast<double>(std::numeric_limits<int32_t>::max())) {
                    return doof::Success<doof::SerialValue>{doof::SerialValue(static_cast<int32_t>(number))};
                }
                if (number >= static_cast<double>(std::numeric_limits<int64_t>::min())
                    && number <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
                    return doof::Success<doof::SerialValue>{doof::SerialValue(static_cast<int64_t>(number))};
                }
            }
            return doof::Success<doof::SerialValue>{doof::SerialValue(number)};
        }
        if (JS_IsArray(value)) {
            JSValue lengthValue = JS_GetPropertyStr(ctx_, value, "length");
            uint32_t length = 0;
            if (JS_IsException(lengthValue) || JS_ToUint32(ctx_, &length, lengthValue) < 0) {
                JS_FreeValue(ctx_, lengthValue);
                return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
            }
            JS_FreeValue(ctx_, lengthValue);

            auto result = std::make_shared<std::vector<doof::SerialValue>>();
            result->reserve(length);
            for (uint32_t index = 0; index < length; ++index) {
                JSValue item = JS_GetPropertyUint32(ctx_, value, index);
                if (JS_IsException(item)) {
                    JS_FreeValue(ctx_, item);
                    return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
                }
                auto converted = jsToJsonValue(item);
                JS_FreeValue(ctx_, item);
                if (doof::is_failure(converted)) {
                    return converted;
                }
                result->push_back(doof::success_value(converted));
            }
            return doof::Success<doof::SerialValue>{doof::SerialValue(std::move(result))};
        }
        if (JS_IsObject(value)) {
            JSPropertyEnum* properties = nullptr;
            uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx_, &properties, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
            }

            auto result = std::make_shared<doof::SerialObject::element_type>();
            for (uint32_t index = 0; index < count; ++index) {
                const char* keyText = JS_AtomToCString(ctx_, properties[index].atom);
                if (keyText == nullptr) {
                    freePropertyNames(properties, count);
                    return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
                }
                std::string key(keyText);
                JS_FreeCString(ctx_, keyText);

                JSValue field = JS_GetProperty(ctx_, value, properties[index].atom);
                if (JS_IsException(field)) {
                    JS_FreeValue(ctx_, field);
                    freePropertyNames(properties, count);
                    return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
                }
                auto converted = jsToJsonValue(field);
                JS_FreeValue(ctx_, field);
                if (doof::is_failure(converted)) {
                    freePropertyNames(properties, count);
                    return converted;
                }
                result->insert_or_assign(key, doof::success_value(converted));
            }
            freePropertyNames(properties, count);
            return doof::Success<doof::SerialValue>{doof::SerialValue(std::move(result))};
        }
        return failure("convert", "value is not JSON-compatible");
    }

    void freePropertyNames(JSPropertyEnum* properties, uint32_t count) {
        if (properties == nullptr) {
            return;
        }
        for (uint32_t index = 0; index < count; ++index) {
            JS_FreeAtom(ctx_, properties[index].atom);
        }
        js_free(ctx_, properties);
    }

    JsValueResult jsonToValue(const doof::SerialValue& value) {
        const auto& storage = doof::serial_storage(value);
        if (std::holds_alternative<std::monostate>(storage)) {
            return doof::Success<JSValue>{JS_NULL};
        }
        if (const auto* boolean = std::get_if<bool>(&storage)) {
            return doof::Success<JSValue>{JS_NewBool(ctx_, *boolean)};
        }
        if (const auto* integer = std::get_if<int32_t>(&storage)) {
            return doof::Success<JSValue>{JS_NewInt32(ctx_, *integer)};
        }
        if (const auto* integer = std::get_if<int64_t>(&storage)) {
            return doof::Success<JSValue>{JS_NewInt64(ctx_, *integer)};
        }
        if (const auto* number = std::get_if<float>(&storage)) {
            if (!std::isfinite(*number)) {
                return doof::Failure<std::shared_ptr<JsError>>{makeError("convert", "JSON argument contains a non-finite number")};
            }
            return doof::Success<JSValue>{JS_NewFloat64(ctx_, static_cast<double>(*number))};
        }
        if (const auto* number = std::get_if<double>(&storage)) {
            if (!std::isfinite(*number)) {
                return doof::Failure<std::shared_ptr<JsError>>{makeError("convert", "JSON argument contains a non-finite number")};
            }
            return doof::Success<JSValue>{JS_NewFloat64(ctx_, *number)};
        }
        if (const auto* text = std::get_if<std::string>(&storage)) {
            return doof::Success<JSValue>{JS_NewStringLen(ctx_, text->data(), text->size())};
        }
        if (const auto* array = std::get_if<doof::SerialArray>(&storage)) {
            JSValue result = JS_NewArray(ctx_);
            if (JS_IsException(result)) {
                return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
            }
            if (*array) {
                for (uint32_t index = 0; index < (*array)->size(); ++index) {
                    auto item = jsonToValue((**array)[index]);
                    if (doof::is_failure(item)) {
                        JS_FreeValue(ctx_, result);
                        return item;
                    }
                    if (JS_SetPropertyUint32(ctx_, result, index, doof::success_value(item)) < 0) {
                        JS_FreeValue(ctx_, result);
                        return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
                    }
                }
            }
            return doof::Success<JSValue>{result};
        }
        if (const auto* object = std::get_if<doof::SerialObject>(&storage)) {
            JSValue result = JS_NewObject(ctx_);
            if (JS_IsException(result)) {
                return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
            }
            if (*object) {
                for (const auto& entry : **object) {
                    auto field = jsonToValue(entry.second);
                    if (doof::is_failure(field)) {
                        JS_FreeValue(ctx_, result);
                        return field;
                    }
                    if (JS_SetPropertyStr(ctx_, result, entry.first.c_str(), doof::success_value(field)) < 0) {
                        JS_FreeValue(ctx_, result);
                        return doof::Failure<std::shared_ptr<JsError>>{exceptionError("convert")};
                    }
                }
            }
            return doof::Success<JSValue>{result};
        }
        return doof::Failure<std::shared_ptr<JsError>>{makeError("convert", "unsupported JSON argument value")};
    }

    std::shared_ptr<JsError> exceptionError(const std::string& fallbackKind = "runtime") {
        JSValue exception = JS_GetException(ctx_);
        std::string name = propertyString(exception, "name");
        std::string message = propertyString(exception, "message");
        std::string stack = propertyString(exception, "stack");

        if (message.empty()) {
            const char* text = JS_ToCString(ctx_, exception);
            if (text != nullptr) {
                message = text;
                JS_FreeCString(ctx_, text);
            }
        }
        if (message.empty()) {
            message = "JavaScript execution failed";
        }

        std::string kind = fallbackKind;
        if (name == "SyntaxError") {
            kind = "syntax";
        }

        JS_FreeValue(ctx_, exception);
        return makeError(kind, message, stack.empty() ? std::nullopt : std::optional<std::string>(stack));
    }

    std::string propertyString(JSValueConst object, const char* name) {
        JSValue property = JS_GetPropertyStr(ctx_, object, name);
        if (JS_IsException(property) || JS_IsUndefined(property) || JS_IsNull(property)) {
            JS_FreeValue(ctx_, property);
            return {};
        }
        const char* text = JS_ToCString(ctx_, property);
        std::string result = text == nullptr ? std::string() : std::string(text);
        if (text != nullptr) {
            JS_FreeCString(ctx_, text);
        }
        JS_FreeValue(ctx_, property);
        return result;
    }

    static std::shared_ptr<JsError> makeError(
        std::string kind,
        std::string message,
        std::optional<std::string> stack = std::nullopt
    ) {
        return std::make_shared<JsError>(std::move(kind), std::move(message), std::move(stack));
    }

    static JsonValueResult failure(std::string kind, std::string message) {
        return doof::Failure<std::shared_ptr<JsError>>{makeError(std::move(kind), std::move(message))};
    }

    void freeValues(std::vector<JSValue>& values) {
        for (auto& value : values) {
            JS_FreeValue(ctx_, value);
        }
        values.clear();
    }
};

} // namespace doof_js
