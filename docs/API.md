# Mosaicraft API

Mosaicraft now keeps two executable entry points:

- `mosaicraft.exe`: command line entry.
- `MosaicraftWebUI.exe`: local Web UI and HTTP API entry.

Both entries call the same core services for mosaic generation, library building,
image inspection, and database maintenance.

## Discovery

`GET /api/endpoints`

Returns the structured API list. Entries marked `legacy: true` are compatibility
endpoints and should not be used by new UI code.

## Health

`GET /api/ping`

Returns `pong`.

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
this endpoint. Keep it only while external callers still need command fallback.
