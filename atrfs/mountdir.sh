#!/bin/bash
#
# mountdir.sh
#
# If you have a directory with a bunch of ATR files, create a mirror
# directory, replacing each ATR file with an ATRFS mount of that file.
#
# Usage:
#  mountdir.sh [source] [target]
#
# The source must exist.  Target must either not exist or be a directory
#

#
# clean()
#
# Unmount all fuse.atrfs mounts under "$1"
#
clean()
{
    cd "$1"
    # Unmount file systems
    # Ugly mangling in case of spaces in file names.
    # Will still break if there are certain special characters in file names.
    mount | grep "$1" | grep "atrfs/mnt" | sed -e 's/ type fuse[.].*//' -e 's/^[^ ]* on //' -e 's/.*/"&"/' -e 's/^/umount /' | sh

    # Safety: There should be no regular files here
    REGULAR=$(find . -type f | wc --lines)
    if [ "${REGULAR}" != 0 ]; then
        echo "ERROR: Found regular files in '$1'"
        return 1
    fi

    # Remove symlinks to non-ATR files
    find . -type l -exec rm '{}' \;

    # Remove all subdirectories
    find * -type d |& grep -v 'No such file' | sort -r | sed -e 's/.*/"&"/' -e 's/^/rmdir /' | sh
    
}

#
# mirror_subdir()
#
# This is called recursively
#
mirror_subdir()
{
    cd "$1"

    # Handle files
    for ENTRY in *; do
        # Case for empty directory:
        if [ "${ENTRY}" == "*" ]; then
            return;
        fi
        [ -d "${ENTRY}" ] && continue # recurse in a second pass
        if hexdump --length 2 "${ENTRY}" | grep -q 0296 || [ $(wc --bytes < "${ENTRY}") == 92160 ]; then
            cd "$2"
            mkdir "${ENTRY}"
            echo "$1/${ENTRY}"
            "${ATRFS}" "$1/${ENTRY}" "${ENTRY}"
            cd "$1"
        else
            cd "$2"
            ln -s "$1"/"${ENTRY}" .
            cd "$1"
        fi
    done

    # Recurse on directories
    for ENTRY in *; do
        # Case for empty directory:
        if [ "${ENTRY}" == "*" ]; then
            return;
        fi
        [ -d "${ENTRY}" ] || break
        cd "$2"
        mkdir "${ENTRY}"
        cd "$1"
        mirror_subdir "$1/${ENTRY}" "$2/${ENTRY}"
    done
    return 0
}

#
# mirror_mount()
#
# Duplicate subdirectories in SOURCE in TARGET
# For all ATR files in SOURCE, create a directory in TARGET and run atrfs
#
mirror_mount()
{
    # Eliminate any relative paths in SOURCE AND TARGET
    CDIR="$(pwd -P)"
    cd "${SOURCE}"
    SOURCE="$(pwd -P)"
    cd "${CDIR}"
    cd "${TARGET}"
    TARGET="$(pwd -P)"

    # Process recursively
    mirror_subdir "${SOURCE}" "${TARGET}"
}

#
# check_target()
#
check_target()
{
    RMDIR="false"
    if [ ! -d "${TARGET}" ]; then
        mkdir "${TARGET}"
        RMDIR="rmdir"
    fi
    if [ ! -d "${TARGET}" ]; then
        echo "ERROR: Unable to create target directory: ${TARGET}"
        return 1
    fi
    cd "${TARGET}"
    COUNT=$(find . | wc --lines)
    if [ "${COUNT}" != 1 ]; then
        echo "ERROR: Target directory not empty: ${TARGET}"
        echo "       $(( COUNT - 1 )) files found"
        return 1
    fi
    cd - >& /dev/null
    return 0
}

#
# check_source()
#
check_source()
{
    if [ ! -d "${SOURCE}" ]; then
        echo "ERROR: Source directory missing: ${SOURCE}"
        return 1
    fi
    cd "${SOURCE}"
    ATRFILES=$((find . -type f -exec hexdump --length 2 '{}' \; | grep 0296;find . -size 92160c) | wc --lines)
    if [ ${ATRFILES} == 0 ]; then
        echo "ERROR: No ATR disk image files found: ${SOURCE}"
        return 1
    fi
    MOUNTS=$(mount | grep fuse | grep -v fusectl | wc --lines)
    if [ ${ATRFILES} -gt $((LIMIT - MOUNTS)) ]; then
        echo "ERROR: Too many ATR files: ${ATRFILES} found, limit is ${LIMIT}"
        [ ${MOUNTS} -gt 0 ] && echo "       Mounts already in use: ${MOUNTS}"
        echo "Use a smaller source or raise the limit in /etc/fuse.conf"
        return 1
    fi
    cd - >&/dev/null
    return 0
}

#
# limits()
#
# FUSE has a default limit of 1000 user mounts
# Check /etc/fuse.conf in case it's different.
#
limits()
{
    LIMIT=$(grep '^ *max_mount *= *[1-9]' /etc/fuse.conf 2>/dev/null | sed -e 's/^[^=]*= *//' -e 's/[^0-9].*//')
    if [ -z "${LIMIT}" ]; then
       LIMIT=1000
    fi
    return 0
}


#
# main()
#
main()
{
    if [ "$1" == "--clean" ]; then
        clean "$2"
        return 0
    fi

    # maybe someday have more comamnd-line options
    ATRFS="$(which atrfs 2>/dev/null)"
    if [ -z "${ATRFS}" ]; then
        echo "ERROR: Unable to find atrfs binary"
        return 1
    fi
    SOURCE="$1"
    TARGET="$2"

    # Sanity check things
    limits || return 1
    check_source "${SOURCE}" || return 1
    check_target "${TARGET}" || return 1
    mirror_mount "${SOURCE}" "${TARGET}"
    cat << EOF

To clean up when you're done:

$0 --clean ${TARGET}

EOF
}

main "$@"
