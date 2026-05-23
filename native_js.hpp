#pragma once

#include "doof_runtime.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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

    doof::Result<doof::JsonValue, std::shared_ptr<JsError>> eval(const std::string& source) {
        beginExecution();
        JSValue value = JS_Eval(ctx_, source.c_str(), source.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);
        endExecution();

        if (JS_IsException(value)) {
            JS_FreeValue(ctx_, value);
            return doof::Result<doof::JsonValue, std::shared_ptr<JsError>>::failure(exceptionError());
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
            return doof::Result<void, std::shared_ptr<JsError>>::failure(exceptionError());
        }

        JS_FreeValue(ctx_, value);
        return doof::Result<void, std::shared_ptr<JsError>>::success();
    }

    doof::Result<doof::JsonValue, std::shared_ptr<JsError>> callJson(
        const std::string& functionName,
        const std::shared_ptr<std::vector<doof::JsonValue>>& args
    ) {
        JSValue global = JS_GetGlobalObject(ctx_);
        JSValue function = JS_GetPropertyStr(ctx_, global, functionName.c_str());
        if (JS_IsException(function)) {
            JS_FreeValue(ctx_, global);
            JS_FreeValue(ctx_, function);
            return doof::Result<doof::JsonValue, std::shared_ptr<JsError>>::failure(exceptionError());
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
                if (converted.isFailure()) {
                    freeValues(jsArgs);
                    JS_FreeValue(ctx_, function);
                    JS_FreeValue(ctx_, global);
                    return doof::Result<doof::JsonValue, std::shared_ptr<JsError>>::failure(converted.error());
                }
                jsArgs.push_back(converted.value());
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
            return doof::Result<doof::JsonValue, std::shared_ptr<JsError>>::failure(exceptionError());
        }

        auto converted = valueToJson(result);
        JS_FreeValue(ctx_, result);
        return converted;
    }

private:
    using JsonValueResult = doof::Result<doof::JsonValue, std::shared_ptr<JsError>>;
    using JsValueResult = doof::Result<JSValue, std::shared_ptr<JsError>>;

    JSRuntime* runtime_ = nullptr;
    JSContext* ctx_ = nullptr;
    JSValue validator_ = JS_UNDEFINED;
    int32_t timeoutMillis_ = 0;
    bool running_ = false;
    std::chrono::steady_clock::time_point deadline_ {};

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
            return JsonValueResult::failure(error);
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
            return JsonValueResult::success(doof::JsonValue(nullptr));
        }
        if (JS_IsBool(value)) {
            return JsonValueResult::success(doof::JsonValue(static_cast<bool>(JS_ToBool(ctx_, value))));
        }
        if (JS_IsString(value)) {
            const char* text = JS_ToCString(ctx_, value);
            if (text == nullptr) {
                return JsonValueResult::failure(exceptionError("convert"));
            }
            std::string copied(text);
            JS_FreeCString(ctx_, text);
            return JsonValueResult::success(doof::JsonValue(std::move(copied)));
        }
        if (JS_IsNumber(value)) {
            double number = 0.0;
            if (JS_ToFloat64(ctx_, &number, value) < 0) {
                return JsonValueResult::failure(exceptionError("convert"));
            }
            if (!std::isfinite(number)) {
                return failure("convert", "value is not JSON-compatible: non-finite number");
            }
            if (std::trunc(number) == number) {
                if (number >= static_cast<double>(std::numeric_limits<int32_t>::min())
                    && number <= static_cast<double>(std::numeric_limits<int32_t>::max())) {
                    return JsonValueResult::success(doof::JsonValue(static_cast<int32_t>(number)));
                }
                if (number >= static_cast<double>(std::numeric_limits<int64_t>::min())
                    && number <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
                    return JsonValueResult::success(doof::JsonValue(static_cast<int64_t>(number)));
                }
            }
            return JsonValueResult::success(doof::JsonValue(number));
        }
        if (JS_IsArray(value)) {
            JSValue lengthValue = JS_GetPropertyStr(ctx_, value, "length");
            uint32_t length = 0;
            if (JS_IsException(lengthValue) || JS_ToUint32(ctx_, &length, lengthValue) < 0) {
                JS_FreeValue(ctx_, lengthValue);
                return JsonValueResult::failure(exceptionError("convert"));
            }
            JS_FreeValue(ctx_, lengthValue);

            auto result = std::make_shared<std::vector<doof::JsonValue>>();
            result->reserve(length);
            for (uint32_t index = 0; index < length; ++index) {
                JSValue item = JS_GetPropertyUint32(ctx_, value, index);
                if (JS_IsException(item)) {
                    JS_FreeValue(ctx_, item);
                    return JsonValueResult::failure(exceptionError("convert"));
                }
                auto converted = jsToJsonValue(item);
                JS_FreeValue(ctx_, item);
                if (converted.isFailure()) {
                    return converted;
                }
                result->push_back(converted.value());
            }
            return JsonValueResult::success(doof::JsonValue(std::move(result)));
        }
        if (JS_IsObject(value)) {
            JSPropertyEnum* properties = nullptr;
            uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx_, &properties, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                return JsonValueResult::failure(exceptionError("convert"));
            }

            auto result = std::make_shared<doof::JsonObject::element_type>();
            for (uint32_t index = 0; index < count; ++index) {
                const char* keyText = JS_AtomToCString(ctx_, properties[index].atom);
                if (keyText == nullptr) {
                    freePropertyNames(properties, count);
                    return JsonValueResult::failure(exceptionError("convert"));
                }
                std::string key(keyText);
                JS_FreeCString(ctx_, keyText);

                JSValue field = JS_GetProperty(ctx_, value, properties[index].atom);
                if (JS_IsException(field)) {
                    JS_FreeValue(ctx_, field);
                    freePropertyNames(properties, count);
                    return JsonValueResult::failure(exceptionError("convert"));
                }
                auto converted = jsToJsonValue(field);
                JS_FreeValue(ctx_, field);
                if (converted.isFailure()) {
                    freePropertyNames(properties, count);
                    return converted;
                }
                result->insert_or_assign(key, converted.value());
            }
            freePropertyNames(properties, count);
            return JsonValueResult::success(doof::JsonValue(std::move(result)));
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

    JsValueResult jsonToValue(const doof::JsonValue& value) {
        const auto& storage = doof::json_storage(value);
        if (std::holds_alternative<std::monostate>(storage)) {
            return JsValueResult::success(JS_NULL);
        }
        if (const auto* boolean = std::get_if<bool>(&storage)) {
            return JsValueResult::success(JS_NewBool(ctx_, *boolean));
        }
        if (const auto* integer = std::get_if<int32_t>(&storage)) {
            return JsValueResult::success(JS_NewInt32(ctx_, *integer));
        }
        if (const auto* integer = std::get_if<int64_t>(&storage)) {
            return JsValueResult::success(JS_NewInt64(ctx_, *integer));
        }
        if (const auto* number = std::get_if<float>(&storage)) {
            if (!std::isfinite(*number)) {
                return JsValueResult::failure(makeError("convert", "JSON argument contains a non-finite number"));
            }
            return JsValueResult::success(JS_NewFloat64(ctx_, static_cast<double>(*number)));
        }
        if (const auto* number = std::get_if<double>(&storage)) {
            if (!std::isfinite(*number)) {
                return JsValueResult::failure(makeError("convert", "JSON argument contains a non-finite number"));
            }
            return JsValueResult::success(JS_NewFloat64(ctx_, *number));
        }
        if (const auto* text = std::get_if<std::string>(&storage)) {
            return JsValueResult::success(JS_NewStringLen(ctx_, text->data(), text->size()));
        }
        if (const auto* array = std::get_if<doof::JsonArray>(&storage)) {
            JSValue result = JS_NewArray(ctx_);
            if (JS_IsException(result)) {
                return JsValueResult::failure(exceptionError("convert"));
            }
            if (*array) {
                for (uint32_t index = 0; index < (*array)->size(); ++index) {
                    auto item = jsonToValue((**array)[index]);
                    if (item.isFailure()) {
                        JS_FreeValue(ctx_, result);
                        return item;
                    }
                    if (JS_SetPropertyUint32(ctx_, result, index, item.value()) < 0) {
                        JS_FreeValue(ctx_, result);
                        return JsValueResult::failure(exceptionError("convert"));
                    }
                }
            }
            return JsValueResult::success(result);
        }
        if (const auto* object = std::get_if<doof::JsonObject>(&storage)) {
            JSValue result = JS_NewObject(ctx_);
            if (JS_IsException(result)) {
                return JsValueResult::failure(exceptionError("convert"));
            }
            if (*object) {
                for (const auto& entry : **object) {
                    auto field = jsonToValue(entry.second);
                    if (field.isFailure()) {
                        JS_FreeValue(ctx_, result);
                        return field;
                    }
                    if (JS_SetPropertyStr(ctx_, result, entry.first.c_str(), field.value()) < 0) {
                        JS_FreeValue(ctx_, result);
                        return JsValueResult::failure(exceptionError("convert"));
                    }
                }
            }
            return JsValueResult::success(result);
        }
        return JsValueResult::failure(makeError("convert", "unsupported JSON argument value"));
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
        return JsonValueResult::failure(makeError(std::move(kind), std::move(message)));
    }

    void freeValues(std::vector<JSValue>& values) {
        for (auto& value : values) {
            JS_FreeValue(ctx_, value);
        }
        values.clear();
    }
};

} // namespace doof_js
