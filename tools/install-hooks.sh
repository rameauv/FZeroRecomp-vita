#!/usr/bin/env sh
# Install the pre-push guard. Hooks live in .git/, which git does not track, so
# a fresh clone has to run this once.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOOK="$ROOT/.git/hooks/pre-push"

cat > "$HOOK" <<'HOOKEOF'
#!/usr/bin/env sh
# Refuse to push anything carrying generated/ -- it is recompiled ROM code, a
# derivative of the commercial ROM. `dev` tracks it deliberately; `main` does
# not. See .gitignore on either branch.
#
# Bypass, if you are certain, with: git push --no-verify
while read -r local_ref local_sha _remote_ref remote_sha; do
    [ "$local_sha" = "0000000000000000000000000000000000000000" ] && continue

    if git ls-tree -r --name-only "$local_sha" -- generated | grep -q .; then
        printf '\npre-push: REFUSED. %s carries generated/, which is recompiled\n' "$local_ref" >&2
        printf 'ROM code and must not be published. Push main instead.\n\n' >&2
        exit 1
    fi

    # Also catch it anywhere in the commits being sent, not just at the tip.
    if [ "$remote_sha" = "0000000000000000000000000000000000000000" ]; then
        range="$local_sha"
    else
        range="$remote_sha..$local_sha"
    fi
    if git rev-list "$range" 2>/dev/null | while read -r c; do
           git ls-tree -r --name-only "$c" -- generated | grep -q . && echo hit
       done | grep -q hit; then
        printf '\npre-push: REFUSED. Commits in %s contain generated/.\n\n' "$range" >&2
        exit 1
    fi
done
exit 0
HOOKEOF

chmod +x "$HOOK"
printf 'Installed %s\n' "$HOOK"
