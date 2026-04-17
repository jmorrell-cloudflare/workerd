// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ser.h"

#include "dom-exception.h"
#include "setup.h"

#include <v8-proxy.h>

namespace workerd::jsg {
namespace {
// Keep in sync with the nativeError serialization tag defined in
// worker-interface.capnp
constexpr uint32_t SERIALIZATION_TAG_NATIVE_ERROR = 10;

// The error tag is serialized as a uint32_t immediately following
// the SERIALIZATION_TAG_NATIVE_ERROR tag in order to more efficiently
// determine the type of error when deserializing so that we can
// construct the appropriate v8::Exception type on deserialization
// without having to expensive string comparison on the error name.
enum class ErrorTag : uint32_t {
  // The UNKNOWN tag is used when the error name is not recognized.
  // When this occurs, we will serialize the name of the error, and
  // when it is deserialized, we will create a generic Error and then
  // set the name to the stored name.
  UNKNOWN,
  ERROR,
  TYPE_ERROR,
  RANGE_ERROR,
  REFERENCE_ERROR,
  SYNTAX_ERROR,
  WASM_COMPILE_ERROR,
  WASM_LINK_ERROR,
  WASM_RUNTIME_ERROR,
  WASM_SUSPEND_ERROR,
  EVAL_ERROR,
  URI_ERROR,
  AGGREGATE_ERROR,
  SUPPRESSED_ERROR,
};

constexpr ErrorTag getErrorTagFromName(jsg::Lock& js, kj::StringPtr str) {
  if (str == "Error"_kj) {
    return ErrorTag::ERROR;
  } else if (str == "TypeError"_kj) {
    return ErrorTag::TYPE_ERROR;
  } else if (str == "RangeError"_kj) {
    return ErrorTag::RANGE_ERROR;
  } else if (str == "ReferenceError"_kj) {
    return ErrorTag::REFERENCE_ERROR;
  } else if (str == "SyntaxError"_kj) {
    return ErrorTag::SYNTAX_ERROR;
  } else if (str == "WasmCompileError"_kj) {
    return ErrorTag::WASM_COMPILE_ERROR;
  } else if (str == "WasmLinkError"_kj) {
    return ErrorTag::WASM_LINK_ERROR;
  } else if (str == "WasmRuntimeError"_kj) {
    return ErrorTag::WASM_RUNTIME_ERROR;
  } else if (str == "WasmSuspendError"_kj) {
    return ErrorTag::WASM_SUSPEND_ERROR;
  } else if (str == "EvalError"_kj) {
    return ErrorTag::EVAL_ERROR;
  } else if (str == "URIError"_kj) {
    return ErrorTag::URI_ERROR;
  } else if (str == "AggregateError"_kj) {
    return ErrorTag::AGGREGATE_ERROR;
  } else if (str == "SuppressedError"_kj) {
    return ErrorTag::SUPPRESSED_ERROR;
  }
  return ErrorTag::UNKNOWN;
}

JsObject toJsError(Lock& js, ErrorTag tag, JsString message) {
  switch (tag) {
    case ErrorTag::ERROR: {
      return JsObject(v8::Exception::Error(message).As<v8::Object>());
    }
    case ErrorTag::TYPE_ERROR: {
      return JsObject(v8::Exception::TypeError(message).As<v8::Object>());
    }
    case ErrorTag::RANGE_ERROR: {
      return JsObject(v8::Exception::RangeError(message).As<v8::Object>());
    }
    case ErrorTag::REFERENCE_ERROR: {
      return JsObject(v8::Exception::ReferenceError(message).As<v8::Object>());
    }
    case ErrorTag::SYNTAX_ERROR: {
      return JsObject(v8::Exception::SyntaxError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_COMPILE_ERROR: {
      return JsObject(v8::Exception::WasmCompileError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_LINK_ERROR: {
      return JsObject(v8::Exception::WasmLinkError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_RUNTIME_ERROR: {
      return JsObject(v8::Exception::WasmRuntimeError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_SUSPEND_ERROR: {
      return JsObject(v8::Exception::WasmSuspendError(message).As<v8::Object>());
    }
    case ErrorTag::EVAL_ERROR: {
      return JsObject(v8::Exception::EvalError(message).As<v8::Object>());
    }
    case ErrorTag::URI_ERROR: {
      return JsObject(v8::Exception::URIError(message).As<v8::Object>());
    }
    case ErrorTag::AGGREGATE_ERROR: {
      return JsObject(v8::Exception::AggregateError(message).As<v8::Object>());
    }
    case ErrorTag::SUPPRESSED_ERROR: {
      return JsObject(v8::Exception::SuppressedError(message).As<v8::Object>());
    }
    case ErrorTag::UNKNOWN: {
      return JsObject(v8::Exception::Error(message).As<v8::Object>());
    }
  }
  return JsObject(v8::Exception::Error(message).As<v8::Object>());
}
}  // namespace

void Serializer::ExternalHandler::serializeFunction(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Function> func) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, func, " could not be cloned.");
}

void Serializer::ExternalHandler::serializeProxy(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Proxy> proxy) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, proxy, " could not be cloned.");
}

Serializer::Serializer(Lock& js, Options options)
    : externalHandler(options.externalHandler),
      treatClassInstancesAsPlainObjects(options.treatClassInstancesAsPlainObjects),
      treatErrorsAsHostObjects(js.isUsingEnhancedErrorSerialization()),
      ser(js.v8Isolate, this) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Serializer must be allocated on the stack");
#endif
  if (!treatClassInstancesAsPlainObjects) {
    prototypeOfObject = js.obj().getPrototype(js);
  }
  if (externalHandler != kj::none) {
    // If we have an ExternalHandler, we'll ask it to serialize host objects.
    ser.SetTreatFunctionsAsHostObjects(true);
    ser.SetTreatProxiesAsHostObjects(true);
  }
  KJ_IF_SOME(version, options.version) {
    KJ_ASSERT(version >= 13, "The minimum serialization version is 13.");
    KJ_ASSERT(jsg::check(ser.SetWriteVersion(version)));
  }
  if (!options.omitHeader) {
    ser.WriteHeader();
  }
}

v8::Maybe<uint32_t> Serializer::GetSharedArrayBufferId(
    v8::Isolate* isolate, v8::Local<v8::SharedArrayBuffer> sab) {
  auto& js = jsg::Lock::from(isolate);
  uint32_t n;
  auto value = JsValue(sab);
  for (n = 0; n < sharedArrayBuffers.size(); n++) {
    // If the SharedArrayBuffer has already been added, return the existing ID for it.
    if (sharedArrayBuffers[n].getHandle(js) == value) {
      return v8::Just(n);
    }
  }
  sharedArrayBuffers.add(jsg::JsRef(js, value));
  sharedBackingStores.add(sab->GetBackingStore());
  return v8::Just(n);
}

namespace {

// True if `name` is a syntactically valid JavaScript identifier, suitable for use after a dot
// in a path string (e.g. `foo.bar`). Non-identifier property names are emitted using the
// quoted-bracket form (e.g. `foo['weird name']`) so the output is always unambiguous.
//
// JavaScript reserved words (like `class` or `new`) ARE syntactically valid in member-access
// position and are rendered here with dot notation — `.class` / `.new` — which is technically
// legal JS and avoids the extra bracket-form overhead for a category most readers will still
// intuit as "a key named `class`".
//
// This is intentionally conservative about the character set: accepts only ASCII identifiers
// plus `$` and `_`, even though JavaScript also permits a broader set of Unicode characters.
// Accepting only ASCII is sufficient for our error-message use case and avoids pulling in
// Unicode tables.
bool isSimpleIdentifier(kj::StringPtr name) {
  if (name.size() == 0) return false;
  auto isStart = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
  };
  auto isCont = [&](char c) { return isStart(c) || (c >= '0' && c <= '9'); };
  if (!isStart(name[0])) return false;
  for (size_t i = 1; i < name.size(); i++) {
    if (!isCont(name[i])) return false;
  }
  return true;
}

// Appends a single-quoted representation of `text` to `out`, escaping characters that would
// otherwise be ambiguous in an error message. Used for property names that aren't simple
// identifiers (e.g. contain spaces, dots, or other non-identifier characters).
//
// We use single quotes (not double quotes) because the full error message wraps the path
// itself in double quotes, like `... at "<path>". ...`. Using single quotes inside the path
// keeps the message unambiguous without needing to escape embedded quotes.
void appendQuoted(kj::Vector<char>& out, kj::StringPtr text) {
  out.add('\'');
  for (char c: text) {
    switch (c) {
      case '\'':
        out.add('\\');
        out.add('\'');
        break;
      case '\\':
        out.add('\\');
        out.add('\\');
        break;
      case '\n':
        out.add('\\');
        out.add('n');
        break;
      case '\r':
        out.add('\\');
        out.add('r');
        break;
      case '\t':
        out.add('\\');
        out.add('t');
        break;
      default:
        out.add(c);
        break;
    }
  }
  out.add('\'');
}

// Appends a single step (property name or index) to `path`. `isArrayIndex` chooses between
// `[N]` (numeric array index), `.foo` (simple identifier — including JS reserved words like
// `class` or `new`), and `['weird name']` (anything else — spaces, non-identifier characters).
void appendPathStep(kj::Vector<char>& path, kj::StringPtr name, bool isArrayIndex) {
  if (isArrayIndex) {
    path.add('[');
    for (char c: name) path.add(c);
    path.add(']');
  } else if (isSimpleIdentifier(name)) {
    path.add('.');
    for (char c: name) path.add(c);
  } else {
    path.add('[');
    appendQuoted(path, name);
    path.add(']');
  }
}

// Maximum recursion depth when searching for the offending object. Bounds worst-case stack
// use for pathological inputs (e.g. very deeply nested JSON). The path reported will simply
// stop at this depth; the fallback "no path" message is used if we can't locate `target`.
constexpr size_t MAX_PATH_SEARCH_DEPTH = 1000;

// Reads a data-property value from `obj` at `key` into `out`, returning true on success.
// This deliberately skips accessor properties (getters/setters) so the walker never invokes
// user-defined JS code on the error path. Proxy traps are also bypassed: if `obj` is a Proxy,
// `GetOwnPropertyDescriptor` consults the Proxy's descriptor trap, but we further require
// that the returned descriptor be a data descriptor (has `value`, not `get`/`set`).
//
// Any failure (property missing, descriptor-trap throw, accessor descriptor) returns false
// without touching `*out`. Exceptions thrown by the trap are caught via the caller's
// enclosing `v8::TryCatch` and must be reset by the caller before the next V8 call.
bool readDataPropertyValue(
    jsg::Lock& js, v8::Local<v8::Object> obj, v8::Local<v8::Name> key, v8::Local<v8::Value>& out) {
  v8::Local<v8::Value> descriptorVal;
  if (!obj->GetOwnPropertyDescriptor(js.v8Context(), key).ToLocal(&descriptorVal)) {
    return false;
  }
  if (!descriptorVal->IsObject()) return false;  // undefined → no such property
  auto descriptor = descriptorVal.As<v8::Object>();

  // Data descriptors expose `value`; accessor descriptors expose `get` / `set`. We only walk
  // data properties.
  auto getStr = v8::String::NewFromUtf8Literal(js.v8Isolate, "get");
  v8::Local<v8::Value> getVal;
  if (descriptor->Get(js.v8Context(), getStr).ToLocal(&getVal) && !getVal->IsUndefined()) {
    return false;  // accessor descriptor — skip
  }
  auto valueStr = v8::String::NewFromUtf8Literal(js.v8Isolate, "value");
  v8::Local<v8::Value> value;
  if (!descriptor->Get(js.v8Context(), valueStr).ToLocal(&value)) return false;
  out = value;
  return true;
}

// Recursive DFS that walks `current`, looking for `target` by v8 object identity. On hit,
// `path` accumulates the step taken to reach the target (appended onto whatever prefix it
// contains on entry). `seen` is a short ordered stack of objects already on the recursion
// path; used for cycle detection with a linear scan since the depth is bounded.
//
// The caller is responsible for providing an enclosing `v8::TryCatch` — any exceptions
// thrown by V8 or user Proxy traps are reset inside this function so the walk remains
// best-effort. We deliberately avoid invoking user-defined accessors (see
// `readDataPropertyValue`) so this walk has no observable side effects on user JS.
bool findPathRecursive(jsg::Lock& js,
    v8::TryCatch& tryCatch,
    v8::Local<v8::Value> current,
    v8::Local<v8::Object> target,
    kj::Vector<char>& path,
    kj::Vector<v8::Local<v8::Object>>& seen,
    size_t depth) {
  // Primitives cannot match (target is always an object) and have no children to recurse into.
  if (!current->IsObject()) return false;

  auto currentObj = current.As<v8::Object>();

  // Identity match: v8::Local<T>::operator== compares the underlying JS identity.
  if (currentObj == target) return true;

  if (depth >= MAX_PATH_SEARCH_DEPTH) return false;

  // Cycle detection: don't recurse into an object we're already inside.
  for (auto& prev: seen) {
    if (prev == currentObj) return false;
  }
  seen.add(currentObj);
  KJ_DEFER(seen.removeLast());

  // Arrays: walk indexed entries in order. Array elements are typically data properties,
  // so we use the fast `Get(index)` path — if a user defined an indexed accessor, we
  // might still fire it, but that's an extremely rare case and we reset any resulting
  // exception below.
  if (currentObj->IsArray()) {
    auto arr = currentObj.As<v8::Array>();
    uint32_t len = arr->Length();
    for (uint32_t i = 0; i < len; i++) {
      v8::Local<v8::Value> elem;
      if (!arr->Get(js.v8Context(), i).ToLocal(&elem)) {
        tryCatch.Reset();
        continue;
      }
      auto savedSize = path.size();
      auto idxStr = kj::str(i);
      appendPathStep(path, idxStr, /* isArrayIndex = */ true);
      if (findPathRecursive(js, tryCatch, elem, target, path, seen, depth + 1)) return true;
      path.resize(savedSize);
    }
  }

  // Walk own enumerable string-keyed properties. This approximates V8's ValueSerializer,
  // which serializes plain objects' own enumerable named keys (no symbols) in insertion
  // order. We use `GetOwnPropertyDescriptor` in `readDataPropertyValue` so accessor
  // properties are silently skipped — V8's serializer would invoke them, but doing so on
  // our diagnostic walk could cause observable side effects or further failures.
  v8::Local<v8::Array> names;
  if (!currentObj
           ->GetOwnPropertyNames(js.v8Context(),
               static_cast<v8::PropertyFilter>(
                   v8::PropertyFilter::ONLY_ENUMERABLE | v8::PropertyFilter::SKIP_SYMBOLS),
               v8::KeyConversionMode::kConvertToString)
           .ToLocal(&names)) {
    tryCatch.Reset();
    return false;
  }

  uint32_t nameCount = names->Length();
  for (uint32_t i = 0; i < nameCount; i++) {
    v8::Local<v8::Value> nameVal;
    if (!names->Get(js.v8Context(), i).ToLocal(&nameVal)) {
      tryCatch.Reset();
      continue;
    }

    // Is this an integer-indexed key? If so, render it with bracket form. For arrays we
    // already walked indexed properties above, so skip to avoid double-walking.
    bool isNumericKey = false;
    {
      v8::Local<v8::Uint32> asUint;
      if (nameVal->ToArrayIndex(js.v8Context()).ToLocal(&asUint)) {
        if (currentObj->IsArray()) continue;  // already walked via arr->Get(i)
        isNumericKey = true;
      } else {
        tryCatch.Reset();
      }
    }

    if (!nameVal->IsName()) continue;  // defensive; SKIP_SYMBOLS ensures strings
    auto nameKey = nameVal.As<v8::Name>();

    v8::Local<v8::Value> child;
    if (!readDataPropertyValue(js, currentObj, nameKey, child)) {
      tryCatch.Reset();
      continue;
    }

    v8::Local<v8::String> nameStr;
    if (!nameVal->ToString(js.v8Context()).ToLocal(&nameStr)) {
      tryCatch.Reset();
      continue;
    }
    auto nameKj = JsValue(nameStr).toString(js);

    auto savedSize = path.size();
    appendPathStep(path, nameKj, /* isArrayIndex = */ isNumericKey);
    if (findPathRecursive(js, tryCatch, child, target, path, seen, depth + 1)) return true;
    path.resize(savedSize);
  }

  return false;
}

}  // namespace

kj::Maybe<kj::String> Serializer::findObjectPath(jsg::Lock& js, v8::Local<v8::Object> target) {
  // Guard the whole walk in a TryCatch so that any V8 exceptions thrown during property
  // enumeration (e.g. from a Proxy's ownKeys trap, or a Proxy's getOwnPropertyDescriptor
  // trap) don't leak out as pending exceptions on the isolate — which would taint the
  // subsequent `js.domException(...)` / `js.throwException(...)` that this function's
  // caller is about to perform.
  v8::TryCatch tryCatch(js.v8Isolate);
  tryCatch.SetVerbose(false);

  // Search each recorded root in order. Thanks to the `writeDepth` guard in `write()`,
  // `writtenRoots` contains only values passed to the outermost public `write()` call(s),
  // not sub-values written recursively from `WriteHostObject` — so paths are always
  // anchored at a meaningful root.
  for (auto& root: writtenRoots) {
    kj::Vector<char> path;
    kj::Vector<v8::Local<v8::Object>> seen;
    v8::Local<v8::Value> rootHandle = root.getHandle(js);

    if (findPathRecursive(js, tryCatch, rootHandle, target, path, seen, 0)) {
      if (path.size() == 0) {
        // Target IS the root. The constructor name of the root would just repeat the type
        // already named in the error message ("of type X at X"), so we suppress the "at"
        // clause entirely. This matches the legacy no-path message shape.
        tryCatch.Reset();
        return kj::none;
      }

      // Use the root value's constructor name as the path prefix so the output reads like
      // an expression you might type: `Array[0].foo`, `Object.nested.bad`, etc. For plain
      // object literals this is "Object"; for the JSRPC args array it's "Array"; for a
      // class instance it's the class name.
      KJ_ASSERT(rootHandle->IsObject(),
          "walker only matched into a non-object root, which shouldn't be possible");
      auto rootName = kj::str(rootHandle.As<v8::Object>()->GetConstructorName());

      kj::Vector<char> out(rootName.size() + path.size());
      for (char c: rootName) out.add(c);
      for (char c: path) out.add(c);
      return kj::heapString(out.asPtr());
    }
  }

  // Reset any residual exception from the walk so the caller's throw is clean.
  tryCatch.Reset();
  return kj::none;
}

void Serializer::throwDataCloneErrorForObject(jsg::Lock& js, v8::Local<v8::Object> obj) {
  // The default error that V8 would generate is "#<TypeName> could not be cloned." -- for some
  // reason, it surrounds the type name in "#<>", which seems bizarre? Let's generate a better
  // error.
  //
  // When possible, we also include the field path where the offending object was encountered
  // within the value(s) passed to `write()`. This walk only runs on the error path, so it has
  // no cost on successful serializations.
  kj::String message;
  KJ_IF_SOME(path, findObjectPath(js, obj)) {
    message = kj::str("Could not serialize object of type \"", obj->GetConstructorName(),
        "\" at \"", path,
        "\". This type does "
        "not support serialization.");
  } else {
    message = kj::str("Could not serialize object of type \"", obj->GetConstructorName(),
        "\". This type does "
        "not support serialization.");
  }
  auto exception = js.domException(kj::str("DataCloneError"), kj::mv(message));
  js.throwException(jsg::JsValue(KJ_ASSERT_NONNULL(exception.tryGetHandle(js))));
}

void Serializer::ThrowDataCloneError(v8::Local<v8::String> message) {
  auto& js = jsg::Lock::current();
  try {
    auto exception = js.domException(kj::str("DataCloneError"), kj::str(message));
    js.v8Isolate->ThrowException(KJ_ASSERT_NONNULL(exception.tryGetHandle(js)));
  } catch (JsExceptionThrown&) {
    // Apparently an exception was thrown during the construction of the DOMException. Most likely
    // we were terminated. In any case, we'll let that exception stay scheduled and propagate back
    // to V8.
  } catch (...) {
    // A KJ exception was thrown, we'll have to convert it to JavaScript and propagate that
    // exception instead.
    throwInternalError(js.v8Isolate, kj::getCaughtExceptionAsKj());
  }
}

bool Serializer::HasCustomHostObject(v8::Isolate* isolate) {
  // By default, V8 will call WriteHostObject() for objects that have internal fields. We only need
  // to override IsHostObject() if we want to treat pure-JS objects differently, which we do if
  // treatClassInstancesAsPlainObjects is false, or if treatErrorsAsHostObjects is true.
  return !treatClassInstancesAsPlainObjects || treatErrorsAsHostObjects;
}

v8::Maybe<bool> Serializer::IsHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  // This is only called if HasCustomHostObject() returned true.
  KJ_ASSERT(!treatClassInstancesAsPlainObjects || treatErrorsAsHostObjects);

  // Any object with internal fields is DEFINITELY a host object!
  if (object->InternalFieldCount() > 0) {
    return v8::Just(true);
  }

  // Native errors are host object if the enhanced_error_serialization compat flag is enalbed.
  if (object->IsNativeError()) {
    return v8::Just(treatErrorsAsHostObjects);
  }

  // If `treatClassInstancesAsPlainObjects` is on (the historical default), then nothing else is
  // a host object. If it has been turned off (e.g. for RPC), then any object that isn't a raw
  // object will be treated as a host object (mainly so that we can error out on these).
  if (treatClassInstancesAsPlainObjects) return v8::Just(false);
  KJ_ASSERT(!prototypeOfObject.IsEmpty());

  // If the object's prototype is Object.prototype, then it is a plain object, which we'll allow
  // to be serialized normally. Otherwise, it is a class instance, which we should treat as a host
  // object. Inside `WriteHostObject()` we will throw DataCloneError due to the object not having
  // internal fields.
#if V8_MAJOR_VERSION >= 15 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 7)
  return v8::Just(object->GetPrototype() != prototypeOfObject);
#else
  // TODO(cleanup): Remove when unnecessary.
  return v8::Just(object->GetPrototypeV2() != prototypeOfObject);
#endif
}

v8::Maybe<bool> Serializer::WriteHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  try {
    jsg::Lock& js = jsg::Lock::from(isolate);

    if (object->IsNativeError()) {
      auto nameStr = js.str("name"_kj);
      auto messageStr = js.str("message"_kj);

      // Get the standard name, message, stack, cause, error, errors properties from
      // the error object.
      writeRawUint32(SERIALIZATION_TAG_NATIVE_ERROR);

      // A mix of ad-hoc and regular serialization. We first serialize
      // the error tag, which is an enum that identifies the type of error
      // for faster/easier deserialization. Then we serialize the message which
      // usually come from the prototype. Then we grab the own properties,
      // serializing the number of them followed by each name and value in
      // sequence.

      jsg::JsObject errorObj(object);
      auto name = errorObj.get(js, nameStr);
      auto nameKjStr = name.toString(js);
      auto tag = getErrorTagFromName(js, nameKjStr);
      writeRawUint32(static_cast<uint32_t>(tag));
      // We only write the name if it is not one of the known error types.
      if (tag == ErrorTag::UNKNOWN) {
        write(js, name);
      }

      write(js, errorObj.get(js, messageStr));

      auto names = errorObj.getPropertyNames(js, KeyCollectionFilter::OWN_ONLY,
          PropertyFilter::ALL_PROPERTIES, IndexFilter::SKIP_INDICES);

      auto obj = js.obj();
      for (size_t n = 0; n < names.size(); n++) {
        auto name = names.get(js, n);
        // The name typically comes from the prototype and therefore
        // do not show up in the own properties of the error object, and the
        // message we want to treat specially since we need it early in the
        // deserialization.
        // Since we already have them serialized above, we can filter them
        // out here.
        if (name.strictEquals(nameStr) || name.strictEquals(messageStr)) continue;

        obj.set(js, name, errorObj.get(js, name));
      }
      write(js, obj);

      return v8::Just(true);
    }

    if (object->InternalFieldCount() != Wrappable::INTERNAL_FIELD_COUNT ||
        !Wrappable::isWorkerdApiObject(object)) {
      KJ_IF_SOME(eh, externalHandler) {
        if (object->IsProxy()) {
          eh.serializeProxy(js, *this, object.As<v8::Proxy>());
          return v8::Just(true);
        } else if (object->IsFunction()) {
          eh.serializeFunction(js, *this, object.As<v8::Function>());
          return v8::Just(true);
        }
      }

      // v8::ValueSerializer by default will send us anything that has internal fields, but this
      // object doesn't appear to match the internal fields expected on a JSG object.
      //
      // We also get here if treatClassInstancesAsPlainObjects is false, and the object is an
      // application-defined class. We don't currently support serializing class instances.
      throwDataCloneErrorForObject(js, object);
    }

    Wrappable* wrappable = reinterpret_cast<Wrappable*>(
        object->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
            static_cast<v8::EmbedderDataTypeTag>(Wrappable::WRAPPED_OBJECT_FIELD_INDEX)));

    // HACK: Although we don't technically know yet that `wrappable` is an `Object`, we know that
    //   only subclasses of `Object` register serializers. So *if* a serializer is found, then this
    //   cast is valid, and the pointer won't be accessed otherwise. We can't do a dynamic_cast
    //   here since `Wrappable` is privately inherited by `Object` and anyway we don't want the
    //   overhead of dynamic_cast.
    // TODO(cleanup): Probably `Wrappable` should contain a bool indicating if it is an `Object`
    //   or not?
    Object* obj = reinterpret_cast<jsg::Object*>(wrappable);

    if (!IsolateBase::from(isolate).serialize(
            Lock::from(isolate), typeid(*wrappable), *obj, *this)) {
      // This type is not serializable.
      throwDataCloneErrorForObject(js, object);
    }

    return v8::Just(true);
  } catch (JsExceptionThrown&) {
    return v8::Nothing<bool>();
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return v8::Nothing<bool>();
  }
}

Serializer::Released Serializer::release() {
  KJ_ASSERT(!released, "The data has already been released.");
  released = true;
  sharedArrayBuffers.clear();
  arrayBuffers.clear();
  auto pair = ser.Release();
  return Released{
    .data = kj::Array(pair.first, pair.second, jsg::SERIALIZED_BUFFER_DISPOSER),
    .sharedArrayBuffers = sharedBackingStores.releaseAsArray(),
    .transferredArrayBuffers = backingStores.releaseAsArray(),
  };
}

void Serializer::transfer(Lock& js, const JsValue& value) {
  KJ_ASSERT(!released, "The data has already been released.");
  // Currently we only allow transfer of ArrayBuffers
  v8::Local<v8::ArrayBuffer> arrayBuffer;
  if (value.isArrayBufferView()) {
    auto view = v8::Local<v8::Value>(value).As<v8::ArrayBufferView>();
    arrayBuffer = view->Buffer();
  } else if (value.isArrayBuffer()) {
    arrayBuffer = v8::Local<v8::Value>(value).As<v8::ArrayBuffer>();
  } else {
    JSG_FAIL_REQUIRE(TypeError, "Object is not transferable");
  }

  // SharedArrayBuffers are not transferable. Per the HTML spec, attempting to
  // transfer a SharedArrayBuffer (or a view backed by one) must throw a
  // DataCloneError. We must check before calling Detach(), which would trigger
  // a fatal V8 CHECK failure on non-detachable buffers.
  JSG_REQUIRE(arrayBuffer->IsDetachable(), DOMDataCloneError, "Object is not transferable.");

  uint32_t n;
  for (n = 0; n < arrayBuffers.size(); n++) {
    // If the ArrayBuffer has already been added, we do not want to try adding it again.
    if (arrayBuffers[n].getHandle(js) == value) {
      return;
    }
  }
  arrayBuffers.add(jsg::JsRef(js, value));

  backingStores.add(arrayBuffer->GetBackingStore());
  check(arrayBuffer->Detach(v8::Local<v8::Value>()));
  ser.TransferArrayBuffer(n, arrayBuffer);
}

void Serializer::write(Lock& js, const JsValue& value) {
  KJ_ASSERT(!released, "The data has already been released.");

  // Only record the outermost call as a root for field-path discovery. Recursive calls from
  // within `WriteHostObject` (native-error serialization, JSG `serialize()` implementations
  // writing nested fields, external handlers writing nested stream/RPC data) would otherwise
  // pollute `writtenRoots` with sub-objects and cause misleading paths to be reported.
  if (writeDepth == 0) {
    writtenRoots.add(jsg::JsRef<JsValue>(js, value));
  }
  ++writeDepth;
  KJ_DEFER(--writeDepth);

  KJ_ASSERT(check(ser.WriteValue(js.v8Context(), value)));
}

Deserializer::ExternalHandler::~ExternalHandler() noexcept(false) {}

Deserializer::Deserializer(Lock& js,
    kj::ArrayPtr<const kj::byte> data,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers,
    kj::Maybe<Options> maybeOptions)
    : totalInputSize(data.size()),
      deser(js.v8Isolate, data.begin(), data.size(), this),
      sharedBackingStores(kj::mv(sharedArrayBuffers)) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Deserializer must be allocated on the stack");
#endif
  init(js, kj::mv(transferredArrayBuffers), kj::mv(maybeOptions));
}

Deserializer::Deserializer(
    Lock& js, Serializer::Released& released, kj::Maybe<Options> maybeOptions)
    : Deserializer(js,
          released.data.asPtr(),
          released.transferredArrayBuffers.asPtr(),
          released.sharedArrayBuffers.asPtr(),
          kj::mv(maybeOptions)) {}

void Deserializer::init(Lock& js,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers,
    kj::Maybe<Options> maybeOptions) {
  auto options = kj::mv(maybeOptions).orDefault({});
  externalHandler = options.externalHandler;
  if (options.readHeader) {
    check(deser.ReadHeader(js.v8Context()));
  }
  preserveStackInErrors = options.preserveStackInErrors;
  KJ_IF_SOME(version, options.version) {
    KJ_ASSERT(version >= 13, "The minimum serialization version is 13.");
    deser.SetWireFormatVersion(version);
  }
  KJ_IF_SOME(arrayBuffers, transferredArrayBuffers) {
    for (auto n: kj::indices(arrayBuffers)) {
      deser.TransferArrayBuffer(n, v8::ArrayBuffer::New(js.v8Isolate, kj::mv((arrayBuffers)[n])));
    }
  }
}

JsValue Deserializer::readValue(Lock& js) {
  return JsValue(check(deser.ReadValue(js.v8Context())));
}

uint Deserializer::readRawUint32() {
  uint32_t result;
  KJ_ASSERT(deser.ReadUint32(&result), "deserialization failure, possible corruption");
  return result;
}

uint64_t Deserializer::readRawUint64() {
  uint64_t result;
  KJ_ASSERT(deser.ReadUint64(&result), "deserialization failure, possible corruption");
  return result;
}

kj::ArrayPtr<const kj::byte> Deserializer::readRawBytes(size_t size) {
  const void* data;
  KJ_ASSERT(deser.ReadRawBytes(size, &data), "deserialization failure, possible corruption");
  return kj::arrayPtr(reinterpret_cast<const kj::byte*>(data), size);
}

kj::ArrayPtr<const kj::byte> Deserializer::readLengthDelimitedBytes() {
  return readRawBytes(readRawUint64());
}

kj::String Deserializer::readRawString(size_t size) {
  return kj::str(readRawBytes(size).asChars());
}

kj::String Deserializer::readLengthDelimitedString() {
  return kj::str(readLengthDelimitedBytes().asChars());
}

v8::MaybeLocal<v8::SharedArrayBuffer> Deserializer::GetSharedArrayBufferFromId(
    v8::Isolate* isolate, uint32_t clone_id) {
  KJ_IF_SOME(backingStores, sharedBackingStores) {
    KJ_ASSERT(clone_id < backingStores.size());
    return v8::SharedArrayBuffer::New(isolate, backingStores[clone_id]);
  }
  return v8::MaybeLocal<v8::SharedArrayBuffer>();
}

v8::MaybeLocal<v8::Object> Deserializer::ReadHostObject(v8::Isolate* isolate) {
  try {
    uint tag = readRawUint32();

    if (tag == SERIALIZATION_TAG_NATIVE_ERROR) {
      auto& js = Lock::from(isolate);
      auto stack = js.str("stack"_kj);

      // The first uint32_t is the error tag, which identifies the type of error.
      auto errorTag = static_cast<ErrorTag>(readRawUint32());
      // If The error tag is UNKNOWN, we will read the name of the error next.
      // If the error is known, we don't both serializing the name.
      kj::Maybe<JsValue> maybeName;
      if (errorTag == ErrorTag::UNKNOWN) {
        maybeName = readValue(js);
      }

      // The next value is the message, which is always present.
      auto messageValue = readValue(js);
      auto messageStr = KJ_UNWRAP_OR(messageValue.tryCast<JsString>(), {
        JSG_FAIL_REQUIRE(
            DOMDataCloneError, "Deserialization failed: error message is not a string");
      });
      // Now let's create the error object based on the tag and message.
      auto obj = toJsError(js, errorTag, messageStr);

      // If we have a name, we set it on the error object. This is not
      // perfect but it gets close enough. Specifically, when the error
      // was serialized, if the user has modified the name or created
      // their own subclass, then we end up having to create just a
      // regular error here and change the name. It is not possible
      // for us here to clone the exact error class that was used,
      // so instanceof checks will not work as expected. But, that's ok.
      KJ_IF_SOME(name, maybeName) {
        // We use defineProperty here since the name is not typically
        // modifiable with set() on error objects.
        obj.defineProperty(js, "name"_kj, name);
      }

      // Now let's read the remaining properties... They were serialized as
      // a plain object with some own properties.
      KJ_IF_SOME(serObj, readValue(js).tryCast<JsObject>()) {
        auto names = serObj.getPropertyNames(js, KeyCollectionFilter::OWN_ONLY,
            PropertyFilter::ALL_PROPERTIES, IndexFilter::SKIP_INDICES);
        for (size_t n = 0; n < names.size(); n++) {
          auto name = names.get(js, n);
          // If the preserveStackInErrors option is false, then we will not
          // restore the serialized stack property if it is included in the
          // serialized output.
          if (!preserveStackInErrors && name.strictEquals(stack)) continue;
          auto value = serObj.get(js, name);
          obj.set(js, name, value);
        }
      }

      v8::Local<v8::Object> ret = obj;
      return ret;
    }

    KJ_IF_SOME(result, IsolateBase::from(isolate).deserialize(Lock::from(isolate), tag, *this)) {
      return result;
    } else {
      // Unknown tag is a platform error, so use KJ assert.
      KJ_FAIL_ASSERT("encountered unknown tag in deserialization", tag);
    }
  } catch (JsExceptionThrown&) {
    return {};
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return {};
  }
}

void SerializedBufferDisposer::disposeImpl(void* firstElement,
    size_t elementSize,
    size_t elementCount,
    size_t capacity,
    void (*destroyElement)(void*)) const {
  free(firstElement);
}

JsValue structuredClone(
    Lock& js, const JsValue& value, kj::Maybe<kj::Array<JsValue>> maybeTransfer) {
  Serializer ser(js);
  KJ_IF_SOME(transfers, maybeTransfer) {
    for (auto& item: transfers) {
      ser.transfer(js, item);
    }
  }
  ser.write(js, value);
  auto released = ser.release();
  Deserializer des(js, released);
  return des.readValue(js);
}

}  // namespace workerd::jsg
