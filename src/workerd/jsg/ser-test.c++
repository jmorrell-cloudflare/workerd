// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "ser.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public jsg::Object, public ContextGlobal {};

kj::Array<kj::byte> lastSerializedData;

struct SerTestContext: public ContextGlobalObject {
  enum class SerializationTag {
    FOO,
    BAR,
    BAZ,
    QUX,
  };

  struct Foo: public jsg::Object {
    uint i;
    Foo(uint i): i(i) {}

    static jsg::Ref<Foo> constructor(jsg::Lock& js, uint i) {
      return js.alloc<Foo>(i);
    }

    int getI() {
      return i;
    }

    JSG_RESOURCE_TYPE(Foo) {
      JSG_READONLY_PROTOTYPE_PROPERTY(i, getI);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeRawUint32(i);
    }
    static jsg::Ref<Foo> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_ASSERT(tag == SerializationTag::FOO);

      // Intentionally deserialize differently so we can detect it.
      return js.alloc<Foo>(deserializer.readRawUint32() + 2);
    }
    JSG_SERIALIZABLE(SerializationTag::FOO);
  };

  struct Bar: public jsg::Object {
    kj::String text;
    Bar(kj::String text): text(kj::mv(text)) {}

    static jsg::Ref<Bar> constructor(jsg::Lock& js, kj::String text) {
      return js.alloc<Bar>(kj::mv(text));
    }

    kj::String getText() {
      return kj::str(text);
    }

    JSG_RESOURCE_TYPE(Bar) {
      JSG_READONLY_PROTOTYPE_PROPERTY(text, getText);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeRawUint64(text.size());
      serializer.writeRawBytes(text.asBytes());
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_ASSERT(tag == SerializationTag::BAR);

      size_t size = deserializer.readRawUint64();
      auto bytes = deserializer.readRawBytes(size);
      // Intentionally deserialize differently so we can detect it.
      return js.alloc<Bar>(kj::str(bytes.asChars(), '!'));
    }
    JSG_SERIALIZABLE(SerializationTag::BAR);
  };

  struct Baz: public jsg::Object {
    bool serializeThrows;
    Baz(bool serializeThrows): serializeThrows(serializeThrows) {}
    static jsg::Ref<Baz> constructor(jsg::Lock& js, bool serializeThrows) {
      return js.alloc<Baz>(serializeThrows);
    }

    JSG_RESOURCE_TYPE(Baz) {}

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      JSG_REQUIRE(!serializeThrows, Error, "throw from serialize()");
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      JSG_FAIL_REQUIRE(Error, "throw from deserialize()");
    }
    JSG_SERIALIZABLE(SerializationTag::BAZ);
  };

  // Qux is like Bar but serializes its string by converting it to a JS value first.
  struct Qux: public jsg::Object {
    kj::String text;
    Qux(kj::String text): text(kj::mv(text)) {}

    static jsg::Ref<Qux> constructor(jsg::Lock& js, kj::String text) {
      return js.alloc<Qux>(kj::mv(text));
    }

    kj::String getText() {
      return kj::str(text);
    }

    JSG_RESOURCE_TYPE(Qux) {
      JSG_READONLY_PROTOTYPE_PROPERTY(text, getText);
    }

    void serialize(
        jsg::Lock& js, jsg::Serializer& serializer, const TypeHandler<kj::String>& stringHandler) {
      // V2 prefers to serialize the string as a JS value.
      serializer.write(js, JsValue(stringHandler.wrap(js, kj::str(text, '?'))));
    }
    static jsg::Ref<Qux> deserialize(Lock& js,
        SerializationTag tag,
        Deserializer& deserializer,
        const TypeHandler<kj::String>& stringHandler) {
      KJ_ASSERT(tag == SerializationTag::QUX);

      return js.alloc<Qux>(
          KJ_ASSERT_NONNULL(stringHandler.tryUnwrap(js, deserializer.readValue(js))));
    }
    JSG_SERIALIZABLE(SerializationTag::QUX);
  };

  JsValue roundTrip(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });

    auto result = ({
      Deserializer deser(js, content);
      deser.readValue(js);
    });

    // Save the last serialization off to the side.
    lastSerializedData = kj::mv(content.data);

    return result;
  }

  JSG_RESOURCE_TYPE(SerTestContext) {
    JSG_NESTED_TYPE(Foo);
    JSG_NESTED_TYPE(Bar);
    JSG_NESTED_TYPE(Baz);
    JSG_NESTED_TYPE(Qux);
    JSG_METHOD(roundTrip);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolate,
    SerTestContext,
    SerTestContext::Foo,
    SerTestContext::Bar,
    SerTestContext::Baz,
    SerTestContext::Qux);

// Define a whole second JSG isolate type that contains "updated" code where Bar no longer wraps
// a string, it wraps an arbitrary value.
struct SerTestContextV2: public ContextGlobalObject {
  enum class SerializationTag { FOO, BAR_OLD, BAZ, QUX, BAR_V2 };

  struct Bar: public jsg::Object {
    JsRef<JsValue> val;
    Bar(JsRef<JsValue> val): val(kj::mv(val)) {}

    static jsg::Ref<Bar> constructor(jsg::Lock& js, JsRef<JsValue> val) {
      return js.alloc<Bar>(kj::mv(val));
    }

    JsRef<JsValue> getVal(Lock& js) {
      return val.addRef(js);
    }

    JSG_RESOURCE_TYPE(Bar) {
      JSG_READONLY_PROTOTYPE_PROPERTY(val, getVal);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      // V2 just writes a value!
      serializer.write(js, JsValue(val.getHandle(js)));
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      if (tag == SerializationTag::BAR_OLD) {
        // Oh, it's an old value.
        size_t size = deserializer.readRawUint64();
        auto bytes = deserializer.readRawBytes(size);

        return js.alloc<Bar>(JsRef<JsValue>(js, js.str(kj::str("old:", bytes.asChars()))));
      } else {
        KJ_ASSERT(tag == SerializationTag::BAR_V2);

        return js.alloc<Bar>(JsRef<JsValue>(js, deserializer.readValue(js)));
      }
    }
    JSG_SERIALIZABLE(SerializationTag::BAR_V2, SerializationTag::BAR_OLD);
  };

  JsValue roundTrip(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });

    auto result = ({
      Deserializer deser(js, content);
      deser.readValue(js);
    });

    // Save the last serialization off to the side.
    lastSerializedData = kj::mv(content.data);

    return result;
  }

  JsValue deserializeLast(Lock& js) {
    Deserializer deser(js, lastSerializedData);
    return deser.readValue(js);
  }

  JSG_RESOURCE_TYPE(SerTestContextV2) {
    JSG_NESTED_TYPE(Bar);
    JSG_METHOD(roundTrip);
    JSG_METHOD(deserializeLast);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolateV2, SerTestContextV2, SerTestContextV2::Bar);

KJ_TEST("serialization") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // Test serializing built-in values.
  e.expectEval("roundTrip(123)", "number", "123");
  e.expectEval("JSON.stringify(roundTrip({foo: 123}))", "string", "{\"foo\":123}");

  // Test serializing host objects.
  e.expectEval("roundTrip(new Foo(123)).i", "number", "125");
  e.expectEval("roundTrip(new Qux(\"hello\")).text", "string", "hello?");
  e.expectEval("roundTrip(new Bar(\"hello\")).text", "string", "hello!");

  // Test throwing from serialize/deserialize
  e.expectEval("roundTrip(new Baz(true)).text", "throws", "Error: throw from serialize()");
  e.expectEval("roundTrip(new Baz(false)).text", "throws", "Error: throw from deserialize()");

  // Let's set up the "new version" of the code.
  Evaluator<SerTestContextV2, SerTestIsolateV2> e2(v8System);

  // This will deserialize the last-serialized bytes from above, where we serialized Bar("hello").
  // However, it is using a "new version" of the code where Bar's serialization has changed, but
  // the old version is still accepted.
  e2.expectEval("deserializeLast().val", "string", "old:hello");

  // Also try round-tripping the new version. It now accepts arbitrary values, not just strings.
  e2.expectEval("roundTrip(new Bar(123)).val", "number", "123");

  // Note that cycles through host objects are correctly serialized!
  //
  // V8 BUG ALERT: The below works if we use `obj` as the root of serialization, but NOT if we
  //   use `bar` as the root. The reason is a flaw in the design of V8's callbacks for parsing
  //   host objects. V8 makes a single callback to the embedder which fully reads the object and
  //   returns a handle. However, this means that V8 cannot put the object into the backreference
  //   table until this callback returns. If, while parsing the object, we encounter a
  //   backreference to the object itself (a cycle), the deserializer will find the backreference
  //   is not in the table and therefore raises an error. This is not a problem for native objects
  //   because V8 allocates the object first, then immediately adds it to the backreference table,
  //   and only then parses its content -- and this is why everything works fine if we start with
  //   a native object as the root, as in this test. The API for host objects needs to be extended
  //   somehow to allow the object to be inserted into the table before parsing its content.
  e2.expectEval("let obj = {i: 321};\n"
                "let bar = new Bar(obj);\n"
                "obj.bar = bar;\n"
                "roundTrip(obj).bar.val.bar.val.bar.val.i",
      "number", "321");
}

// ─────────────────────────────────────────────────────────────────────────────────────
// Tests for the field-path annotation in DataCloneError messages.
//
// These exercise the lazy-walk-on-error machinery in `Serializer` that locates the
// offending object within the root value and reports a JS-like path in the error
// message (e.g. `at "Object.foo.bar"` or `at "Array[0].nested"`). The path prefix is
// the constructor name of the serialization root; the walker-built suffix describes
// how to reach the offender from that root.
//
// We set `treatClassInstancesAsPlainObjects = false` so that class instances and
// null-prototype objects trigger the error path — with the default true, V8 serializes
// them as plain objects and no error occurs.
// ─────────────────────────────────────────────────────────────────────────────────────

struct SerPathTestContext: public ContextGlobalObject {
  // Serialization tags for the types below. Follows the same enum-at-context-level
  // pattern used by SerTestContext so `JSG_SERIALIZABLE` can find the definition.
  enum class SerializationTag { WRAPPER };

  // Resource type whose `serialize()` calls `serializer.write()` on a wrapped value —
  // i.e. performs a recursive write from within `WriteHostObject`. Used to regression-
  // test that recursive writes don't pollute `writtenRoots` (B-1 in the review).
  struct Wrapper: public jsg::Object {
    JsRef<JsValue> inner;
    Wrapper(JsRef<JsValue> inner): inner(kj::mv(inner)) {}

    static jsg::Ref<Wrapper> constructor(jsg::Lock& js, JsRef<JsValue> inner) {
      return js.alloc<Wrapper>(kj::mv(inner));
    }

    JsRef<JsValue> getInner(Lock& js) {
      return inner.addRef(js);
    }

    JSG_RESOURCE_TYPE(Wrapper) {
      JSG_READONLY_PROTOTYPE_PROPERTY(inner, getInner);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      // Recursively write the inner value through the public API. The `writeDepth`
      // counter in `write()` must prevent this from adding to `writtenRoots`, otherwise
      // the path reported for a failure inside `inner` would be anchored at `inner`
      // instead of at the outer caller's root.
      serializer.write(js, JsValue(inner.getHandle(js)));
    }
    static jsg::Ref<Wrapper> deserialize(
        Lock& js, SerializationTag tag, Deserializer& deserializer) {
      return js.alloc<Wrapper>(JsRef<JsValue>(js, deserializer.readValue(js)));
    }
    JSG_SERIALIZABLE(SerializationTag::WRAPPER);
  };

  // Attempts to serialize `value` with `treatClassInstancesAsPlainObjects = false`. On
  // success returns undefined; on failure the DataCloneError propagates to the caller
  // (which then inspects `err.message` in JS).
  void strictSerialize(Lock& js, JsValue value) {
    Serializer ser(js,
        Serializer::Options{
          .treatClassInstancesAsPlainObjects = false,
        });
    ser.write(js, value);
    (void)ser.release();
  }

  JSG_RESOURCE_TYPE(SerPathTestContext) {
    JSG_NESTED_TYPE(Wrapper);
    JSG_METHOD(strictSerialize);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerPathTestIsolate, SerPathTestContext, SerPathTestContext::Wrapper);

KJ_TEST("DataCloneError messages include field paths for serialization failures") {
  Evaluator<SerPathTestContext, SerPathTestIsolate> e(v8System);

  // Helper template — invokes strictSerialize in a try/catch and returns the error message
  // or 'ok'. Used by every assertion below.
  constexpr kj::StringPtr HARNESS = R"(
    function tryClone(v) {
      try {
        strictSerialize(v);
        return 'ok';
      } catch (e) {
        return e.message;
      }
    }
  )"_kj;

  auto run = [&](kj::StringPtr setup, kj::StringPtr expected) {
    e.expectEval(kj::str(HARNESS, setup), "string", expected);
  };

  // ── Case 1: Bad value at a deep path; root is a plain Object ────────────
  // Walker finds the null-proto object at `.foo.bar.baz`; the path prefix is the root's
  // constructor name, "Object".
  run("const bad = Object.create(null);\n"
      "tryClone({foo: {bar: {baz: bad}}})",
      "Could not serialize object of type \"Object\" at \"Object.foo.bar.baz\". "
      "This type does not support serialization.");

  // ── Case 2: Target is the root itself → no "at" clause ──────────────────
  // If we reported a path here it would be just the constructor name, which already
  // appears in the "of type" portion of the message. Suppress the clause entirely —
  // matches the pre-feature message shape for root-level failures.
  run("tryClone(Object.create(null))",
      "Could not serialize object of type \"Object\". "
      "This type does not support serialization.");

  // ── Case 3: Array indices on an array root ──────────────────────────────
  // The root is a v8::Array, so the prefix reads "Array".
  run("const bad = Object.create(null);\n"
      "tryClone([1, 2, bad])",
      "Could not serialize object of type \"Object\" at \"Array[2]\". "
      "This type does not support serialization.");

  // ── Case 4: Mixed array + named access ──────────────────────────────────
  run("const bad = Object.create(null);\n"
      "tryClone({items: ['ok', {nested: bad}]})",
      "Could not serialize object of type \"Object\" at \"Object.items[1].nested\". "
      "This type does not support serialization.");

  // ── Case 5: Cycle — walker must terminate and still find the offender ──
  // Construct a cycle (a↔b) and place the bad value at `a.bad`. The walker needs to
  // handle revisits without looping while still locating the bad value via its
  // direct-property path.
  run("const bad = Object.create(null);\n"
      "const a = {};\n"
      "const b = {};\n"
      "a.b = b; b.a = a; a.bad = bad;\n"
      "tryClone(a)",
      "Could not serialize object of type \"Object\" at \"Object.bad\". "
      "This type does not support serialization.");

  // ── Case 6: Reserved words use plain dot notation ───────────────────────
  // `obj.class` is legal member access in JS, so we render it without bracket-form
  // noise. `.new`, `.return`, etc. similarly render plainly.
  run("const bad = Object.create(null);\n"
      "tryClone({class: bad})",
      "Could not serialize object of type \"Object\" at \"Object.class\". "
      "This type does not support serialization.");

  // ── Case 7: Numeric key on a plain (non-array) object → bracket form ────
  // `{ '0': bad }` must render as `[0]`, not `.0` (which wouldn't parse as JS).
  run("const bad = Object.create(null);\n"
      "tryClone({'0': bad})",
      "Could not serialize object of type \"Object\" at \"Object[0]\". "
      "This type does not support serialization.");

  // ── Case 8: Non-identifier key with apostrophe → escaped ────────────────
  // `appendQuoted` wraps the name in single quotes and backslash-escapes embedded
  // apostrophes, so the key `it's` becomes the path component `'it\'s'`.
  run("const bad = Object.create(null);\n"
      "const payload = {}; payload[\"it's\"] = bad;\n"
      "tryClone(payload)",
      "Could not serialize object of type \"Object\" at \"Object['it\\'s']\". "
      "This type does not support serialization.");

  // ── Case 9: Accessor-backed property — walker skips it (B-2 fix) ────────
  // V8's own ValueSerializer invokes accessor getters when serializing a plain object,
  // so the getter fires once during serialization (the offender is then located for us
  // by V8 itself). The walker, however, must NOT fire the getter a second time when
  // searching for the path — otherwise a getter with side effects would be observed
  // twice by user code. Since our walker skips accessor descriptors entirely, it also
  // cannot locate the offender through that property and falls back to the no-path
  // message. We assert `callCount === 1` (V8's single invocation) rather than 2.
  run("let callCount = 0;\n"
      "const bad = Object.create(null);\n"
      "const payload = {};\n"
      "Object.defineProperty(payload, 'behind', {\n"
      "  enumerable: true,\n"
      "  get() { callCount++; return bad; }\n"
      "});\n"
      "const msg = tryClone(payload);\n"
      "msg + '|callCount=' + callCount",
      "Could not serialize object of type \"Object\". "
      "This type does not support serialization.|callCount=1");

  // ── Case 10: Regression guard for B-1 (recursive-write pollution) ───────
  // Wrapper.serialize() recursively calls serializer.write() on its `inner` value.
  // If the depth guard in write() weren't in place, that recursive call would push
  // `bad` onto `writtenRoots`, and when serialization fails, findObjectPath would
  // match the second entry by identity and report a misleading path pointing at the
  // wrapper's payload directly.
  //
  // The walker cannot descend into JSG resource types (their own-enumerable string
  // properties are empty), so legitimately locating the offender is impossible here.
  // The post-fix expected behavior is the no-path fallback message.
  //
  // If this test starts failing with any `at "..."` clause, it means the depth guard
  // was broken and recursive writes are again polluting `writtenRoots`.
  run("const bad = Object.create(null);\n"
      "const w = new Wrapper(bad);\n"
      "tryClone({wrapped: w})",
      "Could not serialize object of type \"Object\". "
      "This type does not support serialization.");
}

}  // namespace
}  // namespace workerd::jsg::test
