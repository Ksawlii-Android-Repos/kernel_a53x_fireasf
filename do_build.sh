#!/bin/bash

# Wrap to ckbuild.sh
export WP=${WP:-$(realpath $PWD/../)}

# https://github.com/AOSPA/android_vendor_aospa/blob/vauxite/build.sh#L52-L79
# (blow)jobs count
long_opts="jobs:"
getopt_cmd=$(getopt -o hcirv:t:j:m:s:p:bd:zn: --long "$long_opts" \
    -n "$(basename "$0")" -- "$@") || {
    echo -e "\nERROR: Getopt failed. Extra args\n"
    exit 1
}

eval set -- "$getopt_cmd"

# Parse options
while true; do
    case "$1" in
        -j|--jobs|j|jobs)
            export JOBS="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        *)
            shift
            ;;
    esac
done

if [ ! "$JOBS" ]; then
    export JOBS="$(nproc --all)"
fi

if [ ! "$JOBS" == "$(nproc --all)" ]; then
    echo "INFO: Jobs argument passed. Using $JOBS threads"
fi

args=("$@")

contains_u=false
contains_d=false
contains_r=false

for arg in "${args[@]}"; do
    [[ "$arg" == *u* ]] && contains_u=true
    [[ "$arg" == *d* ]] && contains_d=true
    [[ "$arg" == *r* ]] && contains_r=true
done

run_ckbuild() {
    bash kernel_build/ckbuild.sh "$@" -j "$JOBS"
}

if $contains_u && $contains_d; then
    if $contains_r; then
        filtered_args=()
        for arg in "${args[@]}"; do
            filtered_args+=("${arg//d/}")
        done
        run_ckbuild "${filtered_args[@]}"

        filtered_args2=()
        for arg in "${filtered_args[@]}"; do
            filtered_args2+=("${arg//u/}")
        done
        run_ckbuild "${filtered_args2[@]}"
    else
        filtered_args=()
        for arg in "${args[@]}"; do
            filtered_args+=("${arg//d/}")
        done
        run_ckbuild "${filtered_args[@]}"

        filtered_args2=()
        for arg in "${filtered_args[@]}"; do
            filtered_args2+=("${arg//u/}")
            filtered_args2=("${filtered_args2[@]//c/}")
        done
        run_ckbuild "${filtered_args2[@]}"
    fi
elif $contains_d; then
    filtered_args=()
    for arg in "${args[@]}"; do
        filtered_args+=("${arg//d/}")
    done
    run_ckbuild "${filtered_args[@]}"
else
    run_ckbuild "${args[@]}"
fi
