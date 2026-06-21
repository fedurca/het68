# Upstream TinyUSB PR — ready to submit

Prepared PR for the cleanest still-local, generally-useful fix
(**Fix 13**, see [`../../UPSTREAM_DELTA.md`](../../UPSTREAM_DELTA.md)).

- `0001-*.patch` — the commit (forward-ported to TinyUSB `master` / 0.20.1-dev)
- `PR_BODY.md`   — the PR description

## Why this isn't auto-submitted

Opening a PR to `hathach/tinyusb` needs a **fork** + the **GitHub API/web**
(or `gh`). This machine has neither `gh` nor a GitHub token, and the SSH key
only has push rights to `fedurca/*`. So the fork + PR creation must be done by
you (one time).

## Submit steps

```bash
# 1) Fork hathach/tinyusb to your account (web UI "Fork" button, or):
#    gh repo fork hathach/tinyusb --clone=false      # if you install gh + login

# 2) Apply the prepared commit onto upstream master
git clone https://github.com/hathach/tinyusb /tmp/tinyusb-pr
cd /tmp/tinyusb-pr
git checkout -b rp2350-uac2-entity-as-interface
git am /home/gpu/het68/patches/upstream-pr/0001-*.patch

# 3) Push to YOUR fork (SSH works for fedurca/*)
git remote add fork git@github.com:fedurca/tinyusb.git
git push -u fork rp2350-uac2-entity-as-interface

# 4) Open the PR
#    Web: https://github.com/fedurca/tinyusb -> "Compare & pull request"
#    or:  gh pr create --repo hathach/tinyusb \
#           --head fedurca:rp2350-uac2-entity-as-interface \
#           --title "audio: accept class request wIndex addressed to an AS interface" \
#           --body-file /home/gpu/het68/patches/upstream-pr/PR_BODY.md
```

Once you've created the fork, tell me and I can do steps 2–3 (push) from here
over SSH; step 4 (PR creation) still needs `gh`/token/web.
