#!/bin/bash
# This script identifies the root git folder, dynamically extracts submodule paths from
# .gitmodules (if present) without hardcoding them, excludes both submodule directories, any
# directories matching the ".cxx" pattern, and the "./app/mavlink" folder (and its subdirectories),
# finds all .c, .cpp, .h, and .hpp files, and runs clang-format on them.

# Exit immediately if a command exits with a non-zero status.
set -euo pipefail

# Identify the root of the git repository and change to it.
GIT_ROOT=$(git rev-parse --show-toplevel)
cd "$GIT_ROOT"

# Build an array to hold exclusion expressions.
# Each exclusion is represented as a pair: "-path" and the pattern.
exclusions_expr=()

# Dynamically extract submodule paths using git config from .gitmodules.
while IFS= read -r line; do
    # Each line has the format: "submodule.<name>.path <submodule_path>"
    submodule=$(echo "$line" | awk '{print $2}')
    exclusions_expr+=( -path "./$submodule" )
done < <(git config --file .gitmodules --get-regexp path 2>/dev/null || true)

# Exclude any directory (and its subdirectories) matching ".cxx"
exclusions_expr+=( -path "*/.cxx/*" )

exclusions_expr+=( -path "./app/mavlink" -path "./app/wfbngrtl8812/src/main/cpp/include" -path "./app/videonative/src/main/cpp/libs/include" )

# Construct the find command arguments.
find_args=()
if [ ${#exclusions_expr[@]} -gt 0 ]; then
    find_args+=( \( )
    # Iterate over the exclusions_expr array in steps of 2 (each pair: flag and pattern).
    for (( i=0; i<${#exclusions_expr[@]}; i+=2 )); do
        find_args+=( "${exclusions_expr[i]}" "${exclusions_expr[i+1]}" )
        # Add -o between each exclusion pair, except after the last one.
        if (( i+2 < ${#exclusions_expr[@]} )); then
            find_args+=( -o )
        fi
    done
    # Close the grouping and add -prune -o.
    find_args+=( \) -prune -o )
fi

# Append the file-matching criteria.
find_args+=( \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print )

# Execute the find command and run clang-format on each file found.
while IFS= read -r file; do
    echo "Running clang-format on: $file"
    clang-format -i "$file"
done < <(find . "${find_args[@]}")

echo "clang-format completed successfully."
