// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    const { KV } = env;

    console.log(KV);
    await KV.put("test1", "1");
    await KV.put("test2", "2");
    await KV.put("test3", "3");
    await KV.put("test4", "4");

    let res = await KV.list();
    let keys = res.keys.map(key => key.name);

    return new Response(`Received: ${keys}`);
  }
};
