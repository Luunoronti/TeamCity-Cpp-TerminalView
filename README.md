# TeamCity Webhook Ticker (console)

A tiny C++ console app that starts a local HTTP server and reacts to TeamCity webhooks:
- Shows a scrollable feed of builds (newest at the top).
- Colors the **current status** (green/red/gray/yellow) like TeamCity.
- While a build is *running*, shows locally measured elapsed time.
- Shows queue add/remove, start, finish/cancel events.
- Does **not** poll TeamCity; it only reacts to webhooks you configure.

> Endpoint: `POST /webhook` (default port `9876`)

+---------------------------------
E-1234 Erosion_Editor_Win64_Dev
RUNNING (00:05:17)
by alice
+---------------------------------


If a build is not running, the “status” and “build time” lines are hidden.
New events push older ones down (simple terminal “ticker”).

---

## Build

### Linux (Debian/Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
git clone <your-fork-url> teamcity-webhook-ticker
cd teamcity-webhook-ticker
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/teamcity_ticker --bind 0.0.0.0 --port 9876


### Windows (Visual Studio 2022, no WSL)

#### Option A — from Developer Command Prompt

``` bat
:: Open "x64 Native Tools Command Prompt for VS 2022"
git clone <your-fork-url> teamcity-webhook-ticker
cd teamcity-webhook-ticker
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\teamcity_ticker.exe --bind 0.0.0.0 --port 9876
```


#### Option B — inside Visual Studio
File → Open → Folder… (select the repo root). VS detects the CMakeLists.
Select x64-Release and Build (or press Ctrl+Shift+B).
Debug/Run teamcity_ticker with --bind 0.0.0.0 --port 9876 in Debugging args.

> Firewall: open TCP port 9876 on the machine so TeamCity can reach it.

## Configure TeamCity Webhooks

Point your TeamCity (server or the tcWebHooks plugin) at:

```
http://<your-box-ip>:9876/webhook
```

Subscribe to events:
* Build queued / removed from queue
* Build started
* Build finished
* Build interrupted/canceled

The app is tolerant to different payload shapes (official REST-like and tcWebHooks).
It tries these common fields (if present): build.id, build.buildNumber/number,
build.buildType.name/buildTypeName, triggered.user.name/triggeredBy.username,
state (queued/running/finished), status (SUCCESS/FAILURE), canceledInfo, or a top-level "event".

> You don’t need an exact schema match—the server does best-effort parsing and will still display events.


## Quick local test (no TeamCity needed)

In another terminal, send sample events:

``` bash
# Queued
curl -X POST http://127.0.0.1:9876/webhook -H "Content-Type: application/json" -d '{
  "event":"buildQueued",
  "queuedBuild":{"id":"b123","buildType":{"name":"Erosion_Editor_Win64_Dev"},
  "triggered":{"userName":"alice"},"number":"E-123"}
}'

# Started
curl -X POST http://127.0.0.1:9876/webhook -H "Content-Type: application/json" -d '{
  "event":"buildStarted",
  "build":{"id":"b123","state":"running","buildNumber":"E-123",
  "buildType":{"name":"Erosion_Editor_Win64_Dev"},
  "triggered":{"userName":"alice"}}
}'

# Finished (SUCCESS)
curl -X POST http://127.0.0.1:9876/webhook -H "Content-Type: application/json" -d '{
  "event":"buildFinished",
  "build":{"id":"b123","state":"finished","status":"SUCCESS","buildNumber":"E-123",
  "buildType":{"name":"Erosion_Editor_Win64_Dev"},
  "triggered":{"userName":"alice"}}
}'

# Canceled / interrupted
curl -X POST http://127.0.0.1:9876/webhook -H "Content-Type: application/json" -d '{
  "event":"buildInterrupted",
  "build":{"id":"b999","state":"finished","status":"FAILURE","buildNumber":"E-999",
  "buildType":{"name":"Engine_All"},
  "canceledInfo":{"user":{"name":"bob"}}}
}'
```

## Usage

```
teamcity_ticker [--bind <ip>] [--port <port>] [--max-cards <N>]
Defaults: bind=127.0.0.1, port=9876, max-cards=20
Examples:
  teamcity_ticker --bind 0.0.0.0 --port 9876
  teamcity_ticker --max-cards 50
```

* GET / → brief info
* GET /ping → returns OK
* POST /webhook → send TeamCity webhook JSON here

Notes
* Terminal colors use ANSI. On Windows, the app enables Virtual Terminal automatically (falls back to plain text if not available).
* Elapsed time is counted locally from the buildStarted event (or first time we notice state: running).
* The app keeps a compact feed (newest on top). Finished builds remain in the list, but their status/time lines are hidden to match your spec.
