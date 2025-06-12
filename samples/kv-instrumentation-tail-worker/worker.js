// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    const { USERS_NOTIFICATION_CONFIG } = env;

    console.log(USERS_NOTIFICATION_CONFIG);
    await USERS_NOTIFICATION_CONFIG.put("test1", "1");
    await USERS_NOTIFICATION_CONFIG.put("test2", "2");
    await USERS_NOTIFICATION_CONFIG.put("test3", "3");
    await USERS_NOTIFICATION_CONFIG.put("test4", "4");

    let res = await USERS_NOTIFICATION_CONFIG.list();
    let keys = res.keys.map(key => key.name);

    // these arguments should get added to the span
    res = await USERS_NOTIFICATION_CONFIG.list({
      prefix: "te",
      limit: 2,
      cursor: "test1",
    });
    console.log(res);
    keys = res.keys.map(key => key.name);

    return new Response(`Received: ${keys}`);
  }
};
