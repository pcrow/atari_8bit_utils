#!/bin/bash
#
# test.sh
#
# Copyright 2023 Preston Crow
#
# Distributed under the GNU Public License version 2.0
#
# This script is for testing ATRFS.
#


#
# fsinfo()
#
# Input:
#   MNT: Mount point
#
# Output:
#   SECTORS: number of sectors in image
#   SECTORSIZE: sector size in bytes
#   FSTYPE: reporte file system type
#
# Check the .fsinfo file and record parameters
#
fsinfo()
{
    if ! grep -qi "File system information" "${MNT}"/.fsinfo; then
	>&2 echo "Error reading /.fsinfo file (${MNT})"
	ERRORS+=( ".fsinfo" )
	return 1
    fi
    SECTORS=$(grep "^Sectors:" "${MNT}"/.fsinfo | head -n 1 | sed -e 's/.*: *//')
    SECTORSIZE=$(grep "^Sector size:" "${MNT}"/.fsinfo | head -n 1 | sed -e 's/.*: *//' -e 's/ .*//')
    FSTYPE=$(grep "^File system type:" "${MNT}"/.fsinfo | head -n 1 | sed -e 's/.*: *//')
    return 0
}

#
# sectors()
#
# Make sure magic sector files are working.
#
# Input:
#   MNT: Mount point
#   SECTORS: Total number of sectors in image
#
# Tests:
#   sector0 must not exist
#   sector(n+1) must not exist for 'n' sectors in image
#   sector(n) must exist
#   sector10, sector$0a, sector0x0a must all be the same
#
sectors()
{
    R=0
    if cat "${MNT}"/.sector0 |& grep -q "No such file"; then
	true # test passed
    else
	ERRORS+=( ".sector0" )
	R=$((R+1))
    fi
    if cat "${MNT}"/.sector$((SECTORS+1)) |& grep -q "No such file"; then
	true # test passed
    else
	ERRORS+=( ".sector-end" )
	R=$((R+1))
    fi
    BYTES=$( wc --bytes "${MNT}"/.sector1 | sed -e 's/ .*//' )
    if [ -z "${BYTES}" ]; then
	ERRORS+=( ".sector1" )
	R=$((R+1))
    elif [ "${BYTES}" == 128 ]; then
	true # Pass: sector 1 is 128 bytes
    elif [ "${BYTES}" == "${SECTORSIZE}" ]; then
	true # Pass: sector 1 is full density
    else
	ERRORS+=( ".sector1size" )
	R=$((R+1))
    fi
    BYTES=$( wc --bytes "${MNT}"/.sector${SECTORS} | sed -e 's/ .*//' )
    if [ -z "${BYTES}" ]; then
	ERRORS+=( ".sector_n" )
	R=$((R+1))
    elif [ "${BYTES}" == "${SECTORSIZE}" ]; then
	true # Pass: sector 1 is full density
    else
	ERRORS+=( ".sector_n_size" )
	R=$((R+1))
    fi

    if cmp "${MNT}"/.sector0x0a "${MNT}"/.sector010; then
	ERRORS+=( ".sector10hex" )
	R=$((R+1))
    fi
    if cmp "${MNT}"/.sector\$0a "${MNT}"/.sector010; then
	ERRORS+=( ".sector10hex2" )
	R=$((R+1))
    fi
    return ${R}
}

#
# specials()
#
# Make sure special files are working
#
# Input:
#   MNT: Mount point
#
# Tests:
#   .bootinfo has something
#   .bootsectors has something unless sector 1 says zero boot sectors
#   .fsinfo has something
#   .info has something (for main directory info)
#
specials()
{
    R=0

    for FILE in bootinfo fsinfo info; do
	BYTES=$(wc --bytes "${MNT}"/.${FILE} 2>/dev/null | sed -e 's/ .*//')
	if [ -z "${BYTES}" ]; then
	    ERRORS+=( ".${FILE}" )
	    R=$((R+1))
	fi
    done

    BOOTSECTORS=$(hexdump --skip=1 --length=1 -d "${MNT}"/.sector1 2>/dev/null | head -n 1 | sed -e 's/ *$//' -e 's/.* //' -e 's/^00*/0/' -e '/[1-9]/s/^0*//')
    FILE=bootsectors
    BYTES=$(wc --bytes "${MNT}"/.${FILE} 2>/dev/null | sed -e 's/ .*//')
    if [ "${BOOTSECTORS}" == 0 -a "${BYTES}" == 0 ]; then
        true # zero boot sectors for zero-length file
    elif [ -z "${BYTES}" ]; then
	ERRORS+=( ".${FILE}" )
	R=$((R+1))
    elif [ "${BYTES}" == 0 ]; then
	ERRORS+=( ".${FILE}-empty" )
	R=$((R+1))
    elif [ "${BYTES}" == $(( BOOTSECTORS * 128 )) ]; then
        true # All 128 byte sectors; normal
    else
        true # Perhaps a density mix?  Not going to investigate now.
    fi

    return ${R}
}

#
# statfs()
#
# Use 'df' to check file system information and sanity-check it
#
# Input:
#   MNT: Mount point
#   SECTORS: number of sectors in image
#   SECTORSIZE: sector size in bytes
#
# Tests:
#   Expect the total number of bytes to be no larger than the image size.
#   Expect the total number of bytes to be at least 90% of the image size.
#   Expect the used and available bytes together to equal the total bytes.
#
statfs()
{
    SIZES=( $(df --block-size=1 "${MNT}" 2>/dev/null | awk '{print $2,$3,$4}' | tail -n 1) )
    if [ -z "${SIZES[0]}" ]; then
	ERRORS+=( "statfs-not_done" )
	R=$((R+1))
    elif [ ${SIZES[0]} -gt $(( SECTORS * SECTORSIZE )) ]; then
	ERRORS+=( "statfs-toolarge" )
	R=$((R+1))
    elif [ ${SIZES[0]} -ne $(( ${SIZES[1]} + ${SIZES[2]} )) ]; then
	ERRORS+=( "statfs-math" )
	R=$((R+1))
    elif [ ${SIZES[0]} -lt $(( SECTORS * SECTORSIZE * 9 / 10 )) ]; then
	ERRORS+=( "statfs-small" )
	R=$((R+1))
    fi
    return ${R}
}


test()
{
    # Initial tests
    # Must pass these before anything else
    if fsinfo; then
        specials
        statfs
    fi
    if [ ${#ERRORS[*]} -gt 0 ]; then
        return 1
    fi

    # Now create a mirror file system using local files
    # Do a number of operations
    # Compare the results
}

#
# usage()
#
usage()
{
    cat << EOF
$0

Usage:
  $0 [flags] [dir] [atrfiles]...

  Flags:
    --help  (what you see here)

  [dir]:
    This is a scratch directory.  The script will create a
    mount point under this directory as well as a directory
    where it will mirror everything in the ATR file.  It will
    then run a series of file operations on both the files in
    the ATR image and in the local file system.  The two will
    be compared to look for any differences

  [atrfiles]
    Each ATR file will be copied, mounted, and tested one at a time.
EOF
}

#
# cmdline()
#
# Input:
#   $@
#
# Output:
#   SCRATCHDIR - user-specified scratch directory; MNT and DIR are under this
#   MNT - mountpoint
#   DIR - mirror file system directory
#   ATRS - list of ATR files to test with
#   ATR - location to copy ATR files to for testing
#
cmdline()
{
    while echo "$1" | grep -q ^-; do
        case "$1" in
            -h|--help )
                usage
                exit 0
                ;;
            *)
                echo Unknown option: "$1"
                echo ""
                usage
                exit 1
                ;;
        esac
        shift
    done
    if [ -z "$1" ]; then
        echo ERROR: You must specify a directory to work in
        exit 1
    fi
    if [ -f "$1" ]; then
       echo ERROR: You specified an invalid working directory of "$1"
       exit 1
    fi
    if [ ! -d "$1" ]; then
        mkdir "$1" >& /dev/null
        DID_MKDIR=yes
    fi
    if [ ! -d "$1" ]; then
       echo ERROR: Unable to create working directory of "$1"
       exit 1
    fi
    SCRATCHDIR="$1"
    shift
    MNT="${SCRATCHDIR}"/mnt
    DIR="${SCRATCHDIR}"/dir
    ATR="${SCRATCHDIR}"/testcopy.atr
    mkdir "${MNT}" >& /dev/null
    mkdir "${DIR}" >& /dev/null
    if [ ! -d "${MNT}" ]; then
       echo ERROR: Unable to create mount directory: "${MNT}"
       exit 1
    fi
    if [ ! -d "${DIR}" ]; then
       echo ERROR: Unable to create mount directory: "${DIR}"
       exit 1
    fi
    ATRS=( $@ )
    echo First ATR: ${ATRS[0]}
}

#
# cleanup()
#
cleanup()
{
    rm -rf "${DIR}" "${ATR}"
    rmdir "${MNT}"
    [ "${DID_MKDIR}" == "yes" ] && rmdir "${SCRATCHDIR}"
}

#
# test_atr_files()
#
test_atr_files()
{
    for A in "${ATRS[@]}"; do
        # Copy the ATR file so that we can clobber it
        if [ ! -f "${A}" ]; then
            echo ERROR: Unable to locate ATR file: "${A}"
            cleanup
            return 1
        fi
        cp -p "${A}" "${ATR}"
        chmod 644 "${ATR}"

        # Mount the ATR file
        atrfs --name="${ATR}" "${MNT}"
        test
        R=$?
        umount "${MNT}"

        if [ "${R}" -ne 0 ]; then
            echo Tests failed on "${A}"
            for E in ${ERRORS[*]}; do
                echo "  $E"
            done
            return 1
        fi
        echo "Tests passed: ${FSTYPE} image ${A}"
    done
    return 0
}

#
# test_newfs()
#
# Create new images and test them
#
test_newfs()
{
    OPTIONS=(
        "--fs=mydos --secsize=128 --sectors=369"
        "--fs=mydos --secsize=256 --sectors=369"
        "--fs=mydos --secsize=128 --sectors=720"
        "--fs=mydos --secsize=256 --sectors=720"
        "--fs=mydos --secsize=128 --sectors=65535"
        "--fs=mydos --secsize=256 --sectors=65535"
        "--fs=mydos --secsize=128 --sectors=$(( (RANDOM % 256) * (RANDOM % 254) + 370 ))"
        "--fs=mydos --secsize=256 --sectors=$(( (RANDOM % 256) * (RANDOM % 254) + 370 ))"
        "--fs=sparta --volname=Test --secsize=128 --sectors=$(( 6 * 8 ))"
        "--fs=sparta --volname=Test --secsize=256 --sectors=$(( 6 * 4 ))"
        "--fs=sparta --volname=Test --secsize=128 --sectors=$(( (RANDOM % 256) * (RANDOM % 254) + 48 ))"
        "--fs=sparta --volname=Test --secsize=256 --sectors=$(( (RANDOM % 256) * (RANDOM % 254) + 48 ))"
        "--fs=litedos --secsize=128 --sectors=369"
        "--fs=litedos --secsize=256 --sectors=369"
        "--fs=litedos --secsize=128 --sectors=720"
        "--fs=litedos --secsize=256 --sectors=720"
        "--fs=litedos --cluster=4 --secsize=128 --sectors=720"
        "--fs=litedos --cluster=8 --secsize=256 --sectors=720"
        "--fs=litedos --cluster=16 --secsize=128 --sectors=720"
        "--fs=litedos --cluster=32 --secsize=256 --sectors=720"
        "--fs=litedos --secsize=128 --sectors=65535"
        "--fs=litedos --secsize=256 --sectors=65535"
        "--fs=litedos --secsize=128 --sectors=$(( (RANDOM % 256) * (RANDOM % 254) + 370 ))"
        "--fs=litedos --secsize=256 --sectors=$(( (RANDOM % 256) * (RANDOM % 254) + 370 ))"
    )
    for O in "${OPTIONS[@]}"; do
        rm -f "${ATR}"
        atrfs --name="${ATR}" --create $(echo "$O") "${MNT}"
        R=$?
        if [ "${R}" -ne 0 ]; then
            echo Failed to create new file system
            echo Command line: atrfs --name="${ATR}" --create $(echo "$O") "${MNT}"
            return 1
        fi
        test
        R=$?
        umount "${MNT}"

        if [ "${R}" -ne 0 ]; then
            echo Tests failed with "$O"
            for E in ${ERRORS[*]}; do
                echo "  $E"
            done
            return 1
        fi
        echo "Tests passed: ${FSTYPE} options ${O}"
    done
    return 0
}


#
# main()
#
main()
{
    cmdline "$@"
    ERRORS=()

    test_atr_files
    R=$?
    if [ "${R}" -ne 0 ]; then
        return 1
    fi
    echo Tests passed on all pre-existing images
    test_newfs
    if [ "${R}" -ne 0 ]; then
        return 1
    fi
    echo Tests passed on all newly created images
    return 0
}

#
# Run and clean up
#
main "$@"
cleanup
