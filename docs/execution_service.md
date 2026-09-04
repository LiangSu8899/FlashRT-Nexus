# Independent execution service

`execution_http` exposes the existing worker lifecycle, not a second model
scheduler. It accepts a local worker factory or wraps an existing model-runtime
deployment. The provider owns preprocessing, model execution and postprocessing.
Nexus owns admission, execution serialization and retained results. Robot
control and action freshness remain in the consuming application.

```yaml
producer:
  kind: worker
  entry: my_package.provider:build
  config:
    checkpoint: $MODEL_CHECKPOINT
serve:
  transport: execution_http
  host: 127.0.0.1
  port: 8080
  lease_timeout_s: 30
```

Install the provider in the server environment, then run:

```sh
nexus serve deployment.yaml
# Equivalent module entry:
python -m flashrt_nexus.cli serve deployment.yaml
```

The worker factory implements `describe`, `execute`, `reset`, and `close` as
documented in [execution workers](workers.md). For a Native or Python
model-runtime manifest, explicitly set `serve.transport: execution_http`.
Those manifests retain their existing `act_http` default. Native reset currently
closes and reopens the deployment; it can reload weights and recapture. It is not
an inexpensive capsule restore.

## Client

`flashrt_nexus.remote:build` is a public worker factory. Its configuration is:

```yaml
url: http://127.0.0.1:8080
token_env: NEXUS_API_TOKEN
request_timeout_s: 2
execution_timeout_s: 600
poll_interval_s: 0.01
```

Run it inside `ExecutionWorker`, then use `WorkerActionChunks` on the consumer
side. HTTP waits stay in that worker; the robot thread polls the existing C
action-chunk controller. The client environment does not need the server's
model framework, checkpoint or FlashRT installation. It does need a Nexus
execution-only library for the local action queue.

## Version 1 lifecycle

`GET /healthz` reports version, startup epoch, readiness, lease, reset and
outstanding-request state. Authenticated POST endpoints under
`/v1/execution/` are:

| Operation | Contract |
|---|---|
| `acquire` | One exclusive lease per service; returns epoch, lease and provider description |
| `submit` | Epoch, lease, unique request ID, inputs; rejects another outstanding request with 409 |
| `result` | Poll the same request; output remains retained until acknowledged |
| `ack` | Discard a completed result, allowing the next request |
| `reset` | Discard the old result and serialize provider reset behind in-flight work |
| `status` | Poll reset completion |
| `release` | Invalidate the lease and drain/reset before another acquisition |

Clients use fresh request IDs and never retry mutating operations automatically.
A lost submit response is ambiguous: reset explicitly or release/reopen; do not
submit the action twice. Process restart changes the epoch. Old clients cannot
consume a new service's results. Idle lease expiry also requires a new session;
there is no automatic reconnect or resume of robot motion.

There is one outstanding request, including a completed but unacknowledged
result. Execute/reset/close run on one owner executor. Lease expiry discards
results and queues reset, but does **not** interrupt GPU work. Failed reset makes
the service unavailable until restarted. SIGTERM drains before close; a hung
provider can prevent graceful shutdown. These are lifecycle guarantees, not
physical emergency-stop guarantees or hard real-time deadlines.

## Transport scope

JSON scalars and base64 numeric arrays only; no network pickle. Bodies and
responses are limited to 64 MiB. The HTTP adapter has one request handler, a
bounded socket backlog and per-connection timeout. There is no unbounded thread
per connection. Provider calls run separately, so polling does not execute the
model on the HTTP thread.

Loopback can run without a token. Non-loopback binding requires a bearer token
read from `NEXUS_API_TOKEN` (or `serve.token_env`). Use a protected network and a
TLS termination proxy for remote access. Do not expose the plain HTTP listener
to an untrusted network. The client refuses redirects to avoid forwarding
credentials to another destination. This transport prioritizes a reproducible
integration boundary; it is not a zero-copy network performance claim.

The first service is single-consumer. Multi-robot fairness, placement, dynamic
batching and preemptive cancellation are not implemented by this adapter.
