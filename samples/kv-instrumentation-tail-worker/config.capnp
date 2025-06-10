using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "main", worker = .helloWorld),
    (name = "log", worker = .logWorker),
    (name = "kvService", worker = .kvWorker),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ],
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2024-10-14",
  compatibilityFlags = ["experimental", "streaming_tail_worker"],
  streamingTails = ["log"],
  bindings = [
    (name = "KV", kvNamespace = "kvService"),
  ]
);

const logWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "tail.js")
  ],
  compatibilityDate = "2024-10-14",
);

const kvWorker :Workerd.Worker = (
  modules = [
    (name = "kv.js", esModule = embed "kv.js")
  ],
  compatibilityDate = "2024-10-14",
);
