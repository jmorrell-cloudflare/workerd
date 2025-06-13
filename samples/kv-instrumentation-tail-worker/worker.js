// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    const { USERS_NOTIFICATION_CONFIG } = env;

    console.log(USERS_NOTIFICATION_CONFIG);
    await USERS_NOTIFICATION_CONFIG.put("test1", "11111");
    await USERS_NOTIFICATION_CONFIG.put("test2", "22222");
    await USERS_NOTIFICATION_CONFIG.put("test3", "33333");
    await USERS_NOTIFICATION_CONFIG.put("test4", "44444");


    // these arguments should get added to the span
    let res = await USERS_NOTIFICATION_CONFIG.list({
      prefix: "te",
      limit: 2,
      cursor: "test1",
    });
    let keys = res.keys.map(key => key.name);

    let value = await USERS_NOTIFICATION_CONFIG.get("test1");
    // value = await USERS_NOTIFICATION_CONFIG.get("test5");
    // try {
    //   value = await USERS_NOTIFICATION_CONFIG.get("");
    // } catch (e) {
    //   console.log(e);
    // }
    // value = await USERS_NOTIFICATION_CONFIG.get(['test1', 'test2'], 'json');
    console.log(value);

    return new Response(`Received: ${keys}`);
  }
};
