#!/usr/bin/env bash

# Find the repository root directory.
REPO_ROOT=$(git rev-parse --show-toplevel)

# Apply clang-format to all .cpp, .c, .h, and .hpp files within $REPO_ROOT/app/videonative/src/main/cpp,
# excluding the $REPO_ROOT/app/videonative/src/main/cpp/libs/include directory.

find "$REPO_ROOT/app/videonative/src/main/cpp" \
    -path "$REPO_ROOT/app/videonative/src/main/cpp/libs/include" -prune -o \
    \( -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) -exec clang-format -i {} \; \)
