#!/bin/sh
# Prints the guest account's password from .env in the repo root (a line pass=...; gitignored). Used for sudo in the
# guest (vm/ssh-askpass-env.sh | ssh tahoe 'sudo -S -p "" ...') and once to install an SSH key:
#   SSH_ASKPASS=vm/ssh-askpass-env.sh SSH_ASKPASS_REQUIRE=force ssh-copy-id -i ~/.ssh/tahoe_ed25519 tahoe
# macOS sudo -S reads more than one line from a pipe: send the password alone on stdin, never followed by a script.
sed -n 's/^pass=//p' "$(dirname "$0")/../.env" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'   # tolerate stray whitespace
