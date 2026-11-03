#!/bin/sh
# A suite run with every port it used to derive from its pid already taken
# (#177): 8400-9499 listening, accepting and never answering, as another
# program's server would. The suite must ask the OS for free ports.
#   tests/unit/ports_taken.sh <suite binary> [its arguments]
exec python3 -I - "$@" <<'PY'
import resource, socket, subprocess, sys
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
want = 4096 if hard == resource.RLIM_INFINITY else min(hard, 4096)
if soft != resource.RLIM_INFINITY and soft < want: resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
held = []
for port in range(8400, 9500):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port)); s.listen(8); held.append(s)
    except OSError:
        s.close()                       # taken already: just as good
print(f"ports_taken: holding {len(held)} of 1100 ports", flush=True)
sys.exit(subprocess.call(sys.argv[1:]))
PY
