// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming Tail Worker that receives TailStream events and attempts to forward
// them to a "receiver" worker via JSRPC (service binding).
//
// This test exercises the serialization boundary that causes DataCloneError in
// production: the JSRPC serializer uses treatClassInstancesAsPlainObjects=false,
// which rejects objects whose prototype is not Object.prototype.
//
// Each scenario below documents whether the forwarding succeeds or fails, and why.

import * as assert from 'node:assert';

// Track results for each forwarding attempt.
let results = [];

function record(label, status, detail) {
  results.push({ label, status, detail });
}

async function tryForward(env, label, data) {
  try {
    await env.RECEIVER.ingest(label, data);
    record(label, 'ok', null);
  } catch (e) {
    record(label, 'error', `${e.name}: ${e.message}`);
  }
}

export default {
  tailStream(event, env, ctx) {
    // ── Scenario 1: Forward raw onset event ──────────────────────────────
    // The onset event.event is a plain JS object constructed by the C++ ToJs()
    // functions in trace-stream.c++. All properties have Object.prototype.
    // This should succeed.
    ctx.waitUntil(tryForward(env, 'raw-onset', event.event));

    // ── Scenario 2: Forward onset wrapped in a larger object ─────────────
    // This mirrors what Vega does: wrapping event data in a config object.
    ctx.waitUntil(
      tryForward(env, 'wrapped-onset', {
        type: 'invocation',
        onset: event.event,
        traceId: event.spanContext.traceId,
        timestamp: event.timestamp.getTime(),
      })
    );

    // ── Scenario 3: Forward a null-prototype object (constructed in STW) ─
    // This is the suspected root cause of the production DataCloneError.
    // Object.create(null) produces an object with prototype === null, which
    // the JSRPC serializer rejects because prototype !== Object.prototype.
    const nullProto = Object.create(null);
    nullProto.type = 'null-proto-test';
    nullProto.value = 42;
    ctx.waitUntil(tryForward(env, 'null-proto-object', nullProto));

    // ── Scenario 4: Forward an object containing a nested null-prototype ──
    // The outer object is fine, but a nested property has null prototype.
    const nested = Object.create(null);
    nested.x = 1;
    ctx.waitUntil(
      tryForward(env, 'nested-null-proto', {
        type: 'with-nested',
        payload: nested,
      })
    );

    // ── Scenario 5: Forward a class instance ─────────────────────────────
    // Class instances have their class prototype, not Object.prototype.
    // The JSRPC serializer rejects these too.
    class MyData {
      constructor() {
        this.type = 'class-instance';
        this.value = 99;
      }
    }
    ctx.waitUntil(tryForward(env, 'class-instance', new MyData()));

    // ── Scenario 6: Forward an object with a custom prototype chain ──────
    // Object.create({}) creates an object whose prototype is a plain object,
    // but that plain object is NOT Object.prototype — it's an intermediate.
    const customProto = Object.create({ extra: true });
    customProto.type = 'custom-proto';
    ctx.waitUntil(tryForward(env, 'custom-proto-object', customProto));

    // ── Scenario 7: Forward an object with __proto__ explicitly set to null
    // Same as scenario 3 but using the __proto__ assignment pattern.
    const protoNulled = { type: 'proto-nulled', value: 7 };
    protoNulled.__proto__ = null;
    ctx.waitUntil(tryForward(env, 'proto-nulled', protoNulled));

    // Return a handler object for subsequent events.
    return {
      log(event) {
        // ── Scenario 8: Forward raw log event data ─────────────────────
        // Log messages are JSON-parsed by the runtime's ToJs(), so they
        // should be plain objects. This should succeed.
        ctx.waitUntil(tryForward(env, 'raw-log', event.event));
      },

      outcome(event) {
        // ── Scenario 9: Forward raw outcome event ──────────────────────
        ctx.waitUntil(tryForward(env, 'raw-outcome', event.event));
      },

      return(event) {
        // ── Scenario 10: Forward raw return event ──────────────────────
        ctx.waitUntil(tryForward(env, 'raw-return', event.event));
      },

      diagnosticChannel(event) {
        // ── Scenario 11: Forward diagnostic channel event data ─────────
        // The message payload was V8-deserialized. If the original sender
        // published a non-cloneable object, it might have been dropped by
        // the sender-side serializer. But if it survived, it could fail
        // JSRPC here.
        ctx.waitUntil(
          tryForward(env, 'diagnostic-channel-message', {
            channel: event.event.channel,
            message: event.event.message,
          })
        );

        // Also try forwarding just the raw message object.
        ctx.waitUntil(
          tryForward(env, 'diagnostic-channel-raw', event.event.message)
        );
      },

      exception(event) {
        // ── Scenario 12: Forward raw exception event ───────────────────
        ctx.waitUntil(tryForward(env, 'raw-exception', event.event));
      },
    };
  },
};

// Test export — runs after all tail events have been processed.
export const test = {
  async test() {
    // Wait for tail events and JSRPC calls to settle.
    await scheduler.wait(200);

    console.log(`\n${'='.repeat(70)}`);
    console.log('STW DataCloneError Reproduction Results');
    console.log('='.repeat(70));

    const succeeded = results.filter((r) => r.status === 'ok');
    const failed = results.filter((r) => r.status === 'error');

    console.log(`\nSucceeded (${succeeded.length}):`);
    for (const r of succeeded) {
      console.log(`  ✓ ${r.label}`);
    }

    console.log(`\nFailed (${failed.length}):`);
    for (const r of failed) {
      console.log(`  ✗ ${r.label}: ${r.detail}`);
    }
    console.log('');

    // ── Assertions ───────────────────────────────────────────────────────
    // These assertions document the expected behavior based on our analysis.

    function expectOk(label) {
      const r = results.find((r) => r.label === label);
      assert.ok(r, `Missing result for "${label}"`);
      assert.strictEqual(
        r.status,
        'ok',
        `Expected "${label}" to succeed, but got: ${r.detail}`
      );
    }

    function expectError(label, errorSubstring) {
      const r = results.find((r) => r.label === label);
      assert.ok(r, `Missing result for "${label}"`);
      assert.strictEqual(
        r.status,
        'error',
        `Expected "${label}" to fail, but it succeeded`
      );
      if (errorSubstring) {
        assert.ok(
          r.detail.includes(errorSubstring),
          `Expected error for "${label}" to contain "${errorSubstring}", got: ${r.detail}`
        );
      }
    }

    // ── TailStream events (from C++ ToJs()) are plain objects → SAFE ────
    expectOk('raw-onset');
    expectOk('wrapped-onset');
    expectOk('raw-log');
    expectOk('raw-outcome');

    // Return events may not be present for all invocation types (e.g.,
    // exception paths don't produce return events). Check conditionally.
    const returnResult = results.find((r) => r.label === 'raw-return');
    if (returnResult) {
      expectOk('raw-return');
    }

    // Exception events are also plain objects → SAFE.
    expectOk('raw-exception');

    // ── Diagnostic channel messages → SAFE ───────────────────────────────
    // Even when the original sender published a null-prototype object or
    // class instance, the V8 serialization/deserialization round-trip in
    // the diagnostic channel path normalizes them to plain objects with
    // Object.prototype. So they pass JSRPC serialization fine.
    expectOk('diagnostic-channel-message');
    expectOk('diagnostic-channel-raw');

    // ── Non-plain objects constructed in the STW → FAIL ──────────────────
    // The JSRPC serializer (treatClassInstancesAsPlainObjects=false) rejects
    // any object whose prototype is NOT Object.prototype.
    //
    // The error message includes the field path where the offending value was found
    // within the serialized arguments array. For a call `env.RECEIVER.ingest(label, data)`,
    // the call-args array is the serialization root (reported as `Array`), and `data` is
    // the second positional argument (index 1).

    // Null-prototype objects: prototype is null, not Object.prototype.
    // The null-proto object IS the second argument itself, so path = `Array[1]`.
    expectError('null-proto-object', 'at "Array[1]"');
    // The null-proto object is nested at `.payload` of the second argument.
    expectError('nested-null-proto', 'at "Array[1].payload"');
    expectError('proto-nulled', 'at "Array[1]"');

    // Class instances: prototype is the class prototype, not Object.prototype.
    expectError('class-instance', 'at "Array[1]"');

    // Custom prototype chain: prototype is an intermediate object.
    expectError('custom-proto-object', 'at "Array[1]"');

    console.log('\nAll assertions passed.');
  },
};
