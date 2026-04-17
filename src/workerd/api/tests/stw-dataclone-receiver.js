// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// JSRPC receiver — accepts data forwarded by the STW.
// This is analogous to the "processor" worker that the Vega streaming tail
// receptor calls via service binding.

import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

// Module-level storage for received events, accessible by the test export.
let received = [];

export class Receiver extends WorkerEntrypoint {
  // Accept a forwarded tail event. The argument must survive JSRPC serialization.
  async ingest(label, data) {
    received.push({ label, data });
  }
}

// Test export — runs after all tail events have been processed.
// Checks what the STW was able to forward and what failed.
export const test = {
  async test() {
    // Wait for tail events to be fully processed.
    await scheduler.wait(200);

    console.log(`Receiver got ${received.length} events:`);
    for (const { label, data } of received) {
      console.log(`  [${label}] ${JSON.stringify(data)}`);
    }

    // The STW will tell us what it expected to arrive. For now, just verify
    // we received at least the events from the "safe" forwarding paths.
    assert.ok(
      received.length > 0,
      'Expected the receiver to get at least some forwarded events'
    );

    // Verify we received the basic onset forwarding.
    const onsetEvent = received.find((e) => e.label === 'raw-onset');
    assert.ok(onsetEvent, 'Expected to receive a raw onset event');
    assert.strictEqual(onsetEvent.data.type, 'onset');
  },
};
