// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Traced worker — its invocations are tailed by the STW.
// Produces various types of events (logs, diagnostic channels, exceptions)
// to generate different TailStream event types in the tail worker.

import { channel } from 'node:diagnostics_channel';

const ch = channel('test:events');

export default {
  async fetch(request) {
    const url = new URL(request.url);

    switch (url.pathname) {
      case '/basic': {
        // Produces: onset → log → return → outcome
        console.log('hello from traced worker');
        return new Response('ok');
      }

      case '/with-object-log': {
        // Produces a log event where the logged value is a plain object.
        // Log messages are JSON-serialized by the runtime, so any object shape
        // gets flattened to a JSON string. This should always be safe.
        console.log({ key: 'value', nested: { a: 1 } });
        return new Response('ok');
      }

      case '/with-null-proto-log': {
        // Logs an object with a null prototype. The runtime JSON-serializes
        // console.log arguments, so the null prototype should be stripped.
        const obj = Object.create(null);
        obj.foo = 'bar';
        obj.num = 42;
        console.log(obj);
        return new Response('ok');
      }

      case '/with-error': {
        // Produces: onset → exception → return → outcome
        throw new Error('intentional test error');
      }

      case '/with-diagnostic-channel': {
        // Publishes a plain object to a diagnostic channel.
        // The runtime V8-serializes the message, then the STW V8-deserializes it.
        ch.publish({ type: 'test-event', timestamp: Date.now(), data: { x: 1 } });
        return new Response('ok');
      }

      case '/with-diagnostic-channel-null-proto': {
        // Publishes a null-prototype object to a diagnostic channel.
        // The diagnostic channel serializer also uses treatClassInstancesAsPlainObjects=false,
        // so this should FAIL serialization on the sender side (the runtime catches it and
        // logs an exception instead of forwarding the message).
        const msg = Object.create(null);
        msg.type = 'null-proto-event';
        msg.timestamp = Date.now();
        ch.publish(msg);
        return new Response('ok');
      }

      case '/with-diagnostic-channel-class': {
        // Publishes a class instance to a diagnostic channel.
        // Same serializer restrictions apply — this might fail on the sender side.
        class MyEvent {
          constructor() {
            this.type = 'class-event';
            this.timestamp = Date.now();
          }
        }
        ch.publish(new MyEvent());
        return new Response('ok');
      }

      case '/with-diagnostic-channel-nested-null-proto': {
        // Publishes a regular object that CONTAINS a nested null-prototype object.
        // The outer object has Object.prototype, but the inner one doesn't.
        const inner = Object.create(null);
        inner.x = 1;
        ch.publish({ type: 'nested-null-proto', timestamp: Date.now(), payload: inner });
        return new Response('ok');
      }

      default:
        return new Response('not found', { status: 404 });
    }
  },
};

// Test export — triggers various fetch paths to generate different event types.
export const test = {
  async test(controller, env, ctx) {
    // Each fetch generates a full invocation (onset → events → return → outcome)
    // that the STW will receive and try to forward via JSRPC.

    const paths = [
      '/basic',
      '/with-object-log',
      '/with-null-proto-log',
      '/with-error',
      '/with-diagnostic-channel',
      '/with-diagnostic-channel-null-proto',
      '/with-diagnostic-channel-class',
      '/with-diagnostic-channel-nested-null-proto',
    ];

    for (const path of paths) {
      try {
        await env.SERVICE.fetch(`http://placeholder${path}`);
      } catch (e) {
        // /with-error throws intentionally — that's fine, we want the
        // exception event to reach the STW.
      }
    }
  },
};
