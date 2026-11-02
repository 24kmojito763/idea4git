#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    printf 'Usage:\n'
    printf '  %s                 Configure, build, and run tests\n' "$0"
    printf '  %s --run <project>  Build, test, then launch the test build on <project>\n' "$0"
}

run_after_build=0
project_path=''
case "${1:-}" in
    '')
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    --run)
        if [[ $# -ne 2 || ! -d "$2" ]]; then
            usage >&2
            printf 'Error: --run requires an existing project directory.\n' >&2
            exit 2
        fi
        run_after_build=1
        project_path="$(cd "$2" && pwd)"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
build_dir="$repo_root/build/local-test"

for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Error: %s is required. On Ubuntu install the project build dependencies:\n' "$tool" >&2
        printf '  sudo apt install cmake ninja-build g++ qt6-base-dev libqscintilla2-qt6-dev\n' >&2
        exit 1
    fi
done

printf 'Configuring isolated local test build: %s\n' "$build_dir"
cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON

cmake --build "$build_dir" --parallel "${IDEA4GIT_BUILD_JOBS:-2}"
ctest --test-dir "$build_dir" --output-on-failure

printf '\nLocal test binary (not installed): %s/idea4git\n' "$build_dir"

if [[ "$run_after_build" -eq 1 ]]; then
    profile_dir="$build_dir/test-profile"
    mkdir -p "$profile_dir/config" "$profile_dir/data" "$profile_dir/cache"
    printf 'Launching with an isolated test profile on: %s\n' "$project_path"
    XDG_CONFIG_HOME="$profile_dir/config" \
        XDG_DATA_HOME="$profile_dir/data" \
        XDG_CACHE_HOME="$profile_dir/cache" \
        "$build_dir/idea4git" "$project_path"
fi
