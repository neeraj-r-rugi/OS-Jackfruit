

# Overall Mental Model

`engine.c` is **one binary with two execution modes**:

```text id="7l77l2"
engine supervisor <base-rootfs>
engine <client-command> ...
```

So:

* **Supervisor mode** = long-running daemon/server
* **CLI mode** = short-lived client

The CLI never launches containers directly.

It always talks to supervisor.

---

# 1. Supervisor Architecture

The supervisor is the **single authority process**.

It owns:

* all container metadata
* all running child processes
* all logging threads
* all kernel monitor registrations
* all command handling

This means:

> Only supervisor mutates global state.

---

## Supervisor Threads

Recommended final thread model:

```text id="v0n5li"
Main supervisor thread
Signal/reaper thread
1 shared consumer logger thread
N producer threads (1 per container)
```

---

---

# 2. Main Supervisor Thread

Main thread is your **control-plane server**.

It owns:

* UNIX socket listener
* command parsing
* command dispatch
* replies to clients

---

## Main Loop

Since you use UNIX domain `SOCK_DGRAM`:

```text id="7x1g5w"
while running:
    recvfrom()
    parse request
    dispatch handler
    sendto reply
```

No `accept()` needed because datagram sockets preserve message boundaries.

---

## Socket Type

You chose:

```text id="2i8hnk"
AF_UNIX + SOCK_DGRAM
```

This is valid and simple.

Advantages:

* no stream connection handling
* one message = one command
* sender address included automatically

---

## Supervisor Socket Path

Use one fixed socket:

```text id="n0g8ws"
/tmp/engine.sock
```

Supervisor binds this once at startup.

At startup:

remove stale socket if exists.

At shutdown:

unlink it.

⚠️ Important.

---

---

# 3. CLI Architecture

CLI is temporary.

It does:

```text id="k0a1ql"
create own temporary socket
bind unique local path
send request to supervisor
wait reply
print result
unlink local socket
exit
```

---

## Why CLI Needs Own Socket

Because supervisor must reply to correct client.

Example CLI socket:

```text id="d3wy4j"
/tmp/engine_cli_<pid>.sock
```

Each CLI gets unique socket path.

---

## Request-Response Flow

```text id="6bdhzu"
CLI sendto supervisor
↓
supervisor recvfrom gets sender address
↓
supervisor sendto sender
↓
CLI receives reply
```

This fully supports two-way communication without stream sockets.

✅

---

---

# 4. Client Tracking

Important:

> You do NOT keep a permanent client list.

Because normal commands are immediate.

---

## Immediate commands

For:

* start
* stop
* ps
* logs

Flow:

```text id="hqjts7"
request
reply immediately
forget client
```

No tracking needed.

---

## Special case: run

`run` delays reply until container exits.

So only for `run`, metadata stores:

```text id="z5w2oj"
waiting_client_address
has_waiting_client
```

This is temporary delayed reply tracking.

---

---

# 5. Command Model

Recommended command types:

```text id="jzueq3"
START
RUN
STOP
PS
LOGS
```

---

## Supervisor dispatcher

Main thread receives request:

```text id="cwe3n4"
switch(command_type)
```

Routes to proper subsystem.

---

## Structured request recommended

Each request should contain:

```text id="x7ntm5"
command type
container id
rootfs path
command string
soft limit
hard limit
nice value
```

Do not rely on raw shell parsing inside supervisor repeatedly.

---

---

# 6. Metadata System

Supervisor owns container table.

Each container entry stores:

```text id="vwk10a"
container id
host pid
state
start time
rootfs path
log path
soft limit
hard limit
nice
exit reason
stop_requested flag
producer thread handle
waiting client address (for run)
```

---

## Recommended states

Use explicit enum-like states:

```text id="uj1f4u"
STARTING
RUNNING
STOPPING
STOPPED
EXITED
HARD_LIMIT_KILLED
FAILED
```

This avoids ambiguous lifecycle logic.

---

---

# 7. Container Launch Architecture

Container creation happens only inside supervisor.

---

## Launch flow

```text id="3i5snq"
create pipe
clone child
child setup namespaces
parent metadata update
spawn producer thread
register kernel monitor
mark running
```

---

---

# 8. Pipe Creation Before Clone

Pipe must exist before clone:

```text id="gnmmyo"
pipe()
```

Gives:

```text id="4kg7ic"
read end
write end
```

---

## Ownership

| End       | Owner           |
| --------- | --------------- |
| write end | child           |
| read end  | parent/producer |

---

---

# 9. Child Container Setup

Inside child before exec:

---

## Child responsibilities

```text id="f2g0se"
set hostname
mount namespace prep
chroot/pivot_root
mount /proc
dup stdout/stderr to pipe write end
exec command
```

---

## Stdout/Stderr redirection

Both merged into one pipe:

```text id="8fgk3m"
stdout → pipe write end
stderr → pipe write end
```

Recommended simpler design.

✅

---

---

# 10. Producer Thread Architecture

Each container gets one producer thread.

---

## Producer created in same launch function

Yes:

Producer is created immediately after clone succeeds.

Because producer depends on pipe read end.

---

## Producer owns

```text id="tfzt4m"
container id
pipe read fd
buffer pointer
termination flag
```

---

## Producer job

Loop:

```text id="m6q4oz"
read(pipe)
package log entry
push into bounded buffer
```

---

## Producer reads from

Parent-side read fd.

Producer never talks directly to container process.

Only pipe.

---

## Producer exits when

Pipe EOF:

```text id="0hz39f"
read() == 0
```

This means child closed write end.

Container finished.

---

---

# 11. Logging Architecture

Logging is separate subsystem.

---

## Full pipeline

```text id="t8yq1z"
container stdout/stderr
↓
pipe
↓
producer thread
↓
bounded buffer
↓
consumer thread
↓
log file
```

---

---

# 12. Bounded Buffer

Use circular queue.

Each log entry stores:

```text id="zj5d1k"
container id
timestamp
stream type
data
length
```

---

## Why bounded

Prevents unlimited memory growth.

---

## Synchronization

Producer waits if full.

Consumer waits if empty.

---

Separate synchronization from metadata lock.

Very important.

⚠️

---

---

# 13. Consumer Thread

One shared consumer thread is enough.

---

## Consumer job

```text id="5c8y2d"
pop log entry
find target file
append data
flush safely
```

---

Consumer handles all containers.

---

## Why one consumer

Simpler disk write coordination.

---

---

# 14. Log Storage

Persistent per-container log files.

Example:

```text id="qg7x4i"
logs/alpha.log
logs/beta.log
```

---

## Log file opened at container start

Metadata stores:

```text id="uovf50"
log fd
log path
```

Consumer reuses fd.

---

---

# 15. CLI `logs` Command

CLI never reads files directly.

Flow:

```text id="47pg2x"
CLI asks logs alpha
↓
supervisor finds alpha.log
↓
reads file
↓
returns content
```

---

## Important with DGRAM

Because datagram size limited:

Return bounded tail:

```text id="1nb9e2"
last N lines
```

Recommended.

---

---

# 16. Signal Handling

Signal handling must be separate from main socket loop.

---

## SIGTERM / SIGINT

Minimal handler:

```text id="p79q0z"
set running = false
```

Nothing more.

---

## Main loop behavior

`recvfrom()` interrupted:

```text id="h0o8kn"
errno == EINTR
```

Loop checks running flag and exits.

---

## No cleanup inside signal handler

Cleanup happens after loop exits.

---

---

# 17. Signal/Reaper Thread

Dedicated thread handles:

```text id="ekkg7j"
SIGCHLD
SIGTERM
SIGINT
```

Recommended cleaner than raw async handlers.

---

## SIGCHLD responsibilities

```text id="03zq13"
waitpid loop
update metadata
classify exit
mark pending reply if run
```

---

---

# 18. Exit Classification

Required spec logic:

---

## Manual stop

If:

```text id="f4w1qj"
stop_requested == true
```

→ stopped

---

## Hard limit kill

If:

```text id="v0m3de"
SIGKILL and stop_requested == false
```

→ hard_limit_killed

---

## Normal exit

→ exited

---

---

# 19. `run` Command Design

Best design:

> `run = start + delayed final reply`

---

## Supervisor on run

Launch container normally.

Store client address in metadata.

Do not reply immediately.

---

## On child exit

Reaper marks:

```text id="1f6r2e"
pending reply
```

Main thread later sends exit status.

---

## Live streaming?

Spec says optional.

So skip live streaming.

Logs still go to files.

Much simpler.

✅

---

---

# 20. No Permanent Client Registry

Only delayed reply storage for run.

No global client list needed.

---

---

# 21. Shutdown Flow

When supervisor exits:

---

## Ordered shutdown

```text id="3m2m1k"
stop accepting commands
stop containers
wait producers
drain buffer
consumer flushes remaining logs
join threads
close socket
unlink socket
free metadata
exit
```

---

## Consumer exits only when

```text id="ub8zcr"
shutdown requested
AND buffer empty
AND all producers finished
```

Critical for no lost logs.

⚠️

---

---

# 22. Thread Ownership Summary

| Thread        | Owns                      |
| ------------- | ------------------------- |
| Main thread   | socket + command dispatch |
| Signal thread | SIGCHLD/SIGTERM/SIGINT    |
| Producer      | one container pipe        |
| Consumer      | all file writes           |

---

---

# 23. Final System Picture

```text id="76q4j2"
CLI
 ↓
UNIX DGRAM socket
 ↓
Supervisor Main Thread
 ├── Metadata Manager
 ├── Container Launcher
 ├── Kernel Monitor Interface
 ├── Producer Threads
 ├── Consumer Thread
 └── Signal/Reaper Thread
```


