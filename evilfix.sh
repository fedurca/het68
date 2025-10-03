#!/bin/bash

# 1. Stage all new and modified files.
# This might temporarily stage the pico-sdk submodule if its commit has changed.
git add .

# 2. Unstage the pico-sdk submodule.
# This is the key step. It tells Git to IGNORE any changes related to the submodule for this commit.
git reset --quiet pico-sdk

# 3. Commit everything else that is staged.
# --allow-empty prevents an error if the only changes were in the submodule.
# --no-verify bypasses any pre-commit hooks.
git commit --allow-empty --no-verify -m "$(date)"

# 4. Force push the result to the main branch.
git push --force origin main

echo "Done. Submodule changes were ignored."
