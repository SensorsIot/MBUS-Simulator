FROM mcr.microsoft.com/devcontainers/base:debian

# Rename the base image's vscode user to dev, keeping UID 1000 so it matches the
# host user. Host-owned bind mounts are then readable directly and per-user npm
# needs no sudo.
RUN usermod -l dev -d /home/dev -m vscode \
    && groupmod -n dev vscode \
    && sed -i 's/vscode/dev/g' /etc/sudoers.d/vscode || true \
    && mv /etc/sudoers.d/vscode /etc/sudoers.d/dev 2>/dev/null || true

# Drop zsh/oh-my-zsh (~15MB, unused here)
RUN rm -rf /home/*/.oh-my-zsh /root/.oh-my-zsh \
    && apt-get purge -y zsh && apt-get autoremove -y

# Editing/inspection tools only. The ESP-IDF toolchain is deliberately NOT
# installed — firmware is built by GitHub Actions, see .github/workflows/.
# Keep this image lean; do not add idf.py, xtensa/riscv toolchains or pip
# install esp-idf here.
RUN apt-get update && apt-get install -y --no-install-recommends \
        tmux \
        python3 \
        python3-pip \
        python3-venv \
        socat \
        && rm -rf /var/lib/apt/lists/*

# Per-user npm prefix on PATH for every shell, interactive or not. Must be an
# image ENV: Debian's .bashrc returns early for non-interactive shells, so an
# export there would be invisible to `ssh <host> claude ...`, hooks and scripts.
ENV PATH="/home/dev/.npm-global/bin:${PATH}"

USER dev

# UTF-8 so tmux renders block glyphs correctly (claude's splash logo)
RUN echo 'export LANG=C.UTF-8' >> /home/dev/.bashrc

# Persistent claude session: interactive SSH logins attach to a tmux session
# "main" so work survives disconnects. Skips non-interactive shells, shells
# already inside tmux, VS Code terminals, and any login while ~/.no-tmux exists.
RUN printf '%s\n' \
    '' \
    '# --- Persistent claude session (auto-attach tmux on interactive SSH) ---' \
    'if [ -n "$SSH_CONNECTION" ] && [ -z "$TMUX" ] && [ "$TERM_PROGRAM" != "vscode" ] && [ ! -f "$HOME/.no-tmux" ] && command -v tmux >/dev/null 2>&1; then' \
    '    case "$-" in' \
    '        *i*) exec tmux new-session -A -s main -c /workspaces/MBUS-Simulator "claude --dangerously-skip-permissions; exec bash" ;;' \
    '    esac' \
    'fi' \
    >> /home/dev/.bashrc

WORKDIR /workspaces/MBUS-Simulator
RUN echo 'cd /workspaces/MBUS-Simulator 2>/dev/null' >> /home/dev/.bashrc
