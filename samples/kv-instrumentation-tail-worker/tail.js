// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  tailStream(...args) {
    console.log(...args);
    return (...args) => {
      console.log(...args);
    };
  },
};
