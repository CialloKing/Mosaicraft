# Mosaicraft API

Mosaicraft now keeps two executable entry points:

- `mosaicraft.exe`: command line entry.
- `MosaicraftWebUI.exe`: local Web UI and HTTP API entry.

Both entries call the same core services for mosaic generation, library building,
image inspection, and database maintenance.

## Discovery

`GET /api/info`

Returns service version, entry name, structured API status, legacy `/api/run`
status, API contract version, endpoint counts, metadata validation status, and
a compact feature list.

The structured API currently reports:

- `contractVersion`: semantic version of the HTTP/service contract exposed by
  discovery.
- `contractMajorVersion`: major compatibility version. Additive metadata or
  endpoint changes should keep this stable; breaking request/response changes
  must increment it.
- `compatibility`: stability label for generated clients and UI adapters.
- `stable`: whether the current contract is intended for regular callers.

`GET /api/endpoints`

Returns the structured API list. Each entry contains:

- `method`: HTTP method or method set.
- `methods`: machine-readable HTTP method list.
- `path`: endpoint path.
- `httpPattern`: server route pattern used by the local HTTP adapter.
- `operation`: stable core API operation name.
- `requestShape`: how the endpoint maps HTTP data into the core request
  (`none`, `body`, `query`, `jobId`, or `legacyCommand`).
- `description`: short endpoint description.
- `category`: functional group, such as `jobs`, `database`, or `inspect`.
- `requestFields`: common request fields accepted by the endpoint.
- `requiredFields`: fields that callers must provide for the operation to
  succeed. Fields omitted from this list have core defaults or are optional.
- `fieldAliases`: accepted alternate JSON or query names for canonical request
  fields, for example `dbPath` also accepts `db`.
- `queryKeys`: machine-readable query string keys accepted by the endpoint.
- `acceptedQueryKeys`: complete query keys accepted by the HTTP adapter,
  including canonical field names and aliases.
- `successStatus`: normal HTTP status code for a successful response.
- `responseKey`: primary JSON payload key returned on success, when the
  response has one.
- `errorStatuses`: HTTP status codes the structured handler may return for
  expected failures.
- `errorShapes`: JSON error envelope variants used by the endpoint. Common
  values are `serviceResult` (`ok`, `exitCode`, `message`), `apiError`
  (`ok`, `message`), `jobError` (`ok`, `message`, `job`), and `payloadResult`
  (`ok`, `message`, `exitCode`, plus the endpoint payload key).
- `errorResponseKeys`: primary payload keys that may also appear in an error
  response, such as `job`, `export`, or `purge`.
- `errorResponses`: paired error response contracts containing `status`,
  `shape`, and optional `responseKey`.
- `sideEffects`: whether the endpoint can modify files, jobs, or databases.
- `longRunning`: whether callers should prefer asynchronous handling or polling.
- `legacy`: whether this endpoint is only for compatibility.
- `enabled`: whether the endpoint is currently available.

Entries marked `legacy: true` are compatibility endpoints and should not be used
by new UI code. The `enabled` field reports whether a compatibility endpoint is
currently available.

## Health

`GET /api/ping`

Returns the standard JSON health response:

```json
{
  "ok": true,
  "message": "pong"
}
```

## Errors

Malformed JSON request bodies return HTTP 400 with the standard result shape:

```json
{
  "ok": false,
  "exitCode": 1,
  "message": "invalid JSON body: ..."
}
```

## Jobs

`POST /api/jobs/mosaic`

Starts an asynchronous mosaic job.

`POST /api/jobs/build`

Starts an asynchronous library build job.

`GET /api/jobs`

Lists known jobs.

`DELETE /api/jobs`

Removes finished jobs from the in-memory job list. Running and queued jobs are
kept.

`GET /api/jobs/{id}`

Returns a single job snapshot.

`DELETE /api/jobs/{id}`

Cancels a queued job. Running jobs are not interrupted and return HTTP 409.

## Mosaic

`POST /api/mosaic`

Runs a mosaic request synchronously. New UI code should prefer
`POST /api/jobs/mosaic` for long-running work.

## Inspect

`GET|POST /api/inspect`

Inspects a source image and optionally reports database match coverage.

Common JSON fields:

```json
{
  "imagePath": "target.jpg",
  "dbPath": "library/mosaicraft.db"
}
```

## Database

`GET|POST /api/db/stats`

Returns database size, LAB ranges, brightness buckets, and coverage gaps.

`GET|POST /api/db/health`

Returns health warnings and recommendations.

`GET|POST /api/db/usage`

Returns usage statistics.

Common JSON fields:

```json
{
  "dbPath": "library/mosaicraft.db",
  "limit": 50,
  "showUnused": false
}
```

`POST /api/db/usage/export`

Exports used images ordered by total tile count. This endpoint has filesystem
side effects and requires `confirm: true`.

```json
{
  "dbPath": "library/mosaicraft.db",
  "outputDir": "used-images",
  "confirm": true
}
```

`GET|POST /api/db/purge`

Previews or removes orphan database records. The default is dry-run. Actual
deletion requires `dryRun: false` and `confirm: true`.

```json
{
  "dbPath": "library/mosaicraft.db",
  "dryRun": true,
  "confirm": false
}
```

## Legacy

`POST /api/run`

Compatibility endpoint for old command-style callers. The Web UI no longer uses
this endpoint. It is disabled by default; set
`MOSAICRAFT_ENABLE_LEGACY_RUN=1` before starting `MosaicraftWebUI.exe` to enable
compatibility mode.
