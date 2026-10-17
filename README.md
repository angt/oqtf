# oqtf

`oqtf` runs a command for a fixed time. When the time ends, it stops the
command and everything it spawned: children, grandchildren, daemons.
Nothing escapes, not even with `setsid` or a double fork.

`oqtf` itself is never stopped. With `-s`, the command is never stopped
either. Only its children.

Linux only. Needs cgroups v2.

## Build

```
make
```

Install it with `make install`.

## Use

Give a time, then a command:

```
oqtf 1m make
```

A build can not hang forever. `oqtf` stops it after one minute.

The time is mandatory.

```
oqtf 500ms curl api
oqtf 2m make
oqtf 1h30m make
```

With no command, `oqtf` starts your shell, from the `SHELL` environment
variable:

```
oqtf 2m30s
```

The shell stays. Every program you start from it lives for 2m30s at most.

## Options

```
-s, --shell    The command never stops, only its children
-h, --help     Show this help
```

Options work before or after the time. Use `-s` for a shell:

```
oqtf 30m -s
```

`-s` keeps the command alive. Every program it runs gets thirty
minutes, on its own.

`-s` supervises what runs inside its cgroup, and nothing else. It does
not fit commands that hand work to a daemon somewhere else: a `tmux`
server you attached to is outside the group, and a `tmux` server that
`oqtf` started is inside, so it too stops when its time is up.

`--` ends the options. Everything after it is the command.

## How it works

`oqtf` makes a new cgroup and moves itself into it. The command starts in
the same cgroup, and everything it spawns is born there too.

Every fraction of a second, `oqtf` walks the cgroup and its subgroups.
A process older than the time gets `SIGKILL`. The cgroup is the complete
list, so no process can hide. Before signalling a process, `oqtf` pins
it in `/proc`, so a process that died in the meantime can not make
`oqtf` signal an unrelated process that reused the pid.

The command gets `SIGTERM` at the time limit, then `SIGKILL` two seconds
later. A command that made itself impossible to signal is stopped
anyway: the final kill goes through `cgroup.kill`, which the kernel
enforces on every process in the group, whatever its privileges.

With root, the command runs in its own cgroup namespace, over a cgroup
filesystem that starts at its group. It can not move itself, or anything
it spawns, out of the group. Without root, that confinement is not
available: the command stays in the group by itself, and the timeout
still applies.

When the command exits, `oqtf` stops what is left, removes any subgroup,
moves itself out, and removes the cgroup.

Signals sent to `oqtf` itself are ignored: acting on them would leave
the command running with nobody to clean up. A terminal interrupt goes
to the command as usual, and `oqtf` then cleans up.

## Permissions

`oqtf` needs to create a cgroup inside your current one. With `sudo` it
always works:

```
sudo oqtf 1m make
```

Without `sudo` it works when your current cgroup is writable and may
have subgroups. That depends on how your session was set up; if it is
not, `oqtf` says so before running anything. Root is also what buys the
command its confinement.

## Exit code

`oqtf` exits with the code of the command. Other cases:

```
124    the time fired
127    the command can not run, like `timeout`
2      bad usage, for example no time
1      `oqtf` itself failed
```

A command stopped by a signal gives 128 plus the signal number: 143 for
`SIGTERM`, 137 for `SIGKILL`.
