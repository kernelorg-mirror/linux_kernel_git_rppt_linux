#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

DIR="$(dirname "$(readlink -f "$0")")"
. "${DIR}"/../kselftest/ktap_helpers.sh

ktap_print_header

if [[ $(id -u) -ne 0 ]]; then
  ktap_skip_all "This test must be run as root"
  exit "$KSFT_SKIP"
fi

nr_hugepgs=$(cat /proc/sys/vm/nr_hugepages)

fault_limit_file=limit_in_bytes
reservation_limit_file=rsvd.limit_in_bytes
fault_usage_file=usage_in_bytes
reservation_usage_file=rsvd.usage_in_bytes

if [[ "$1" == "-cgroup-v2" ]]; then
  cgroup2=1
  fault_limit_file=max
  reservation_limit_file=rsvd.max
  fault_usage_file=current
  reservation_usage_file=rsvd.current
fi

if [[ $cgroup2 ]]; then
  cgroup_path=$(mount -t cgroup2 | head -1 | awk '{print $3}')
  if [[ -z "$cgroup_path" ]]; then
    cgroup_path=$(mktemp -d)
    mount -t cgroup2 none $cgroup_path
    do_umount=1
  fi
  echo "+hugetlb" >$cgroup_path/cgroup.subtree_control
else
  cgroup_path=$(mount -t cgroup | grep ",hugetlb" | awk '{print $3}')
  if [[ -z "$cgroup_path" ]]; then
    cgroup_path=$(mktemp -d)
    mount -t cgroup memory,hugetlb $cgroup_path
    do_umount=1
  fi
fi
export cgroup_path

function cleanup() {
  if [[ $cgroup2 ]]; then
    echo $$ >$cgroup_path/cgroup.procs
  else
    echo $$ >$cgroup_path/tasks
  fi

  if [[ -e /mnt/huge ]]; then
    rm -rf /mnt/huge/*
    umount /mnt/huge 2>/dev/null || true
    rmdir /mnt/huge 2>/dev/null || true
  fi
  if [[ -e $cgroup_path/hugetlb_cgroup_test ]]; then
    rmdir $cgroup_path/hugetlb_cgroup_test
  fi
  if [[ -e $cgroup_path/hugetlb_cgroup_test1 ]]; then
    rmdir $cgroup_path/hugetlb_cgroup_test1
  fi
  if [[ -e $cgroup_path/hugetlb_cgroup_test2 ]]; then
    rmdir $cgroup_path/hugetlb_cgroup_test2
  fi
  echo 0 >/proc/sys/vm/nr_hugepages
}

function final_cleanup() {
  cleanup

  if [[ $do_umount ]]; then
    umount $cgroup_path
    rmdir $cgroup_path
  fi

  echo "$nr_hugepgs" > /proc/sys/vm/nr_hugepages
}
trap final_cleanup EXIT

function check_equal() {
  local expected="$1"
  local actual="$2"
  local error="$3"

  if [[ "$expected" != "$actual" ]]; then
    ktap_print_msg "expected ($expected) != actual ($actual): $error"
    test_failed=1
  fi
}

function get_machine_hugepage_size() {
  hpz=$(grep -i hugepagesize /proc/meminfo)
  kb=${hpz:14:-3}
  mb=$(($kb / 1024))
  echo $mb
}

MB=$(get_machine_hugepage_size)

function setup_cgroup() {
  local name="$1"
  local cgroup_limit="$2"
  local reservation_limit="$3"

  mkdir $cgroup_path/$name

  echo "$cgroup_limit" >$cgroup_path/$name/hugetlb.${MB}MB.$fault_limit_file
  echo "$reservation_limit" > \
    $cgroup_path/$name/hugetlb.${MB}MB.$reservation_limit_file

  if [ -e "$cgroup_path/$name/cpuset.cpus" ]; then
    echo 0 >$cgroup_path/$name/cpuset.cpus
  fi
  if [ -e "$cgroup_path/$name/cpuset.mems" ]; then
    echo 0 >$cgroup_path/$name/cpuset.mems
  fi
}

function wait_for_file_value() {
  local path="$1"
  local expect="$2"
  local max_tries=60

  if [[ ! -r "$path" ]]; then
    ktap_print_msg "cannot read '$path', missing or permission denied"
    return 1
  fi

  for ((i=1; i<=max_tries; i++)); do
    local cur="$(cat "$path")"
    if [[ "$cur" == "$expect" ]]; then
      return 0
    fi
    sleep 1
  done

  ktap_print_msg "timeout waiting for $path to become '$expect'"
  return 1
}

function wait_for_hugetlb_memory_to_get_depleted() {
  local cgroup="$1"
  local path="$cgroup_path/$cgroup/hugetlb.${MB}MB.$reservation_usage_file"

  wait_for_file_value "$path" "0"
}

function wait_for_hugetlb_memory_to_get_reserved() {
  local cgroup="$1"
  local size="$2"
  local path="$cgroup_path/$cgroup/hugetlb.${MB}MB.$reservation_usage_file"

  wait_for_file_value "$path" "$size"
}

function wait_for_hugetlb_memory_to_get_written() {
  local cgroup="$1"
  local size="$2"
  local path="$cgroup_path/$cgroup/hugetlb.${MB}MB.$fault_usage_file"

  wait_for_file_value "$path" "$size"
}

function write_hugetlbfs_and_get_usage() {
  local cgroup="$1"
  local size="$2"
  local populate="$3"
  local write="$4"
  local path="$5"
  local method="$6"
  local private="$7"
  local expect_failure="$8"
  local reserve="$9"

  reservation_failed=0
  oom_killed=0
  hugetlb_difference=0
  reserved_difference=0

  local hugetlb_usage=$cgroup_path/$cgroup/hugetlb.${MB}MB.$fault_usage_file
  local reserved_usage=$cgroup_path/$cgroup/hugetlb.${MB}MB.$reservation_usage_file

  local hugetlb_before=$(cat $hugetlb_usage)
  local reserved_before=$(cat $reserved_usage)

  output=$(mktemp)
  if [[ "$method" == "1" ]] || [[ "$method" == 2 ]] ||
    [[ "$private" == "-r" ]] && [[ "$expect_failure" != 1 ]]; then

    bash write_hugetlb_memory.sh "$size" "$populate" "$write" \
      "$cgroup" "$path" "$method" "$private" "-l" "$reserve" 2>&1 | tee $output &

    local write_result=$?
    local write_pid=$!

    until grep -q -i "DONE" $output; do
      if ! ps $write_pid > /dev/null 2>&1; then
        ktap_print_msg "write_to_hugetlbfs died unexpectedly"
        test_failed=1
        rm -f $output
        return
      fi
      sleep 0.5
    done

    if [[ "$populate" == "-o" ]] || [[ "$write" == "-w" ]]; then
      wait_for_hugetlb_memory_to_get_written "$cgroup" "$size"
    elif [[ "$reserve" != "-n" ]]; then
      wait_for_hugetlb_memory_to_get_reserved "$cgroup" "$size"
    else
      sleep 0.5
    fi
  else
    bash write_hugetlb_memory.sh "$size" "$populate" "$write" \
      "$cgroup" "$path" "$method" "$private" "$reserve"
    local write_result=$?

    if [[ "$reserve" != "-n" ]]; then
      wait_for_hugetlb_memory_to_get_reserved "$cgroup" "$size"
    fi
  fi

  rm -f $output

  if [[ "$write_result" == 1 ]]; then
    reservation_failed=1
  fi

  if [[ "$write_result" == 135 ]] || [[ "$write_result" == 137 ]]; then
    oom_killed=1
  fi

  local hugetlb_after=$(cat $hugetlb_usage)
  local reserved_after=$(cat $reserved_usage)

  hugetlb_difference=$(($hugetlb_after - $hugetlb_before))
  reserved_difference=$(($reserved_after - $reserved_before))
}

function cleanup_hugetlb_memory() {
  local cgroup="$1"
  if [[ "$(pgrep -f write_to_hugetlbfs)" != "" ]]; then
    killall -2 --wait write_to_hugetlbfs 2>/dev/null
    wait_for_hugetlb_memory_to_get_depleted $cgroup
  fi

  if [[ -e /mnt/huge ]]; then
    rm -rf /mnt/huge/*
    umount /mnt/huge 2>/dev/null || true
    rmdir /mnt/huge 2>/dev/null || true
  fi
}

function run_test() {
  local size=$(($1 * ${MB} * 1024 * 1024))
  local populate="$2"
  local write="$3"
  local cgroup_limit=$(($4 * ${MB} * 1024 * 1024))
  local reservation_limit=$(($5 * ${MB} * 1024 * 1024))
  local nr_hugepages="$6"
  local method="$7"
  local private="$8"
  local expect_failure="$9"
  local reserve="${10}"

  hugetlb_difference=0
  reserved_difference=0
  reservation_failed=0
  oom_killed=0

  echo "$nr_hugepages" >/proc/sys/vm/nr_hugepages

  setup_cgroup "hugetlb_cgroup_test" "$cgroup_limit" "$reservation_limit"

  mkdir -p /mnt/huge
  mount -t hugetlbfs -o pagesize=${MB}M none /mnt/huge

  write_hugetlbfs_and_get_usage "hugetlb_cgroup_test" "$size" "$populate" \
    "$write" "/mnt/huge/test" "$method" "$private" "$expect_failure" \
    "$reserve"

  cleanup_hugetlb_memory "hugetlb_cgroup_test"

  local final_hugetlb=$(cat $cgroup_path/hugetlb_cgroup_test/hugetlb.${MB}MB.$fault_usage_file)
  local final_reservation=$(cat $cgroup_path/hugetlb_cgroup_test/hugetlb.${MB}MB.$reservation_usage_file)

  check_equal "0" "$final_hugetlb" "final hugetlb usage is not zero"
  check_equal "0" "$final_reservation" "final reservation usage is not zero"
}

# Construct a descriptive test name from the parameters.
function test_name() {
  local label="$1"
  local method="$2"
  local private="$3"
  local populate="$4"
  local reserve="$5"

  local method_name
  case "$method" in
    0) method_name="hugetlbfs" ;;
    1) method_name="mmap" ;;
    2) method_name="shmem" ;;
  esac

  local attrs="$method_name"
  [[ "$private" == "-r" ]] && attrs="$attrs,private" || attrs="$attrs,shared"
  [[ "$populate" == "-o" ]] && attrs="$attrs,populate"
  [[ "$reserve" == "-n" ]] && attrs="$attrs,noreserve"

  echo "$label ($attrs)"
}

cleanup

for populate in "" "-o"; do
  for method in 0 1 2; do
    for private in "" "-r"; do
      for reserve in "" "-n"; do

        # Skip mmap(MAP_HUGETLB | MAP_SHARED). Doesn't seem to be supported.
        if [[ "$method" == 1 ]] && [[ "$private" == "" ]]; then
          continue
        fi

        # Skip populated shmem tests. Doesn't seem to be supported.
        if [[ "$method" == 2 ]] && [[ "$populate" == "-o" ]]; then
          continue
        fi

        if [[ "$method" == 2 ]] && [[ "$reserve" == "-n" ]]; then
          continue
        fi

        # --- Test normal case ---
        cleanup
        test_failed=0
        run_test 5 "$populate" "" 10 10 10 "$method" "$private" "0" "$reserve"

        if [[ "$populate" == "-o" ]]; then
          check_equal "$((5 * $MB * 1024 * 1024))" "$hugetlb_difference" \
            "hugetlb charge mismatch"
        else
          check_equal "0" "$hugetlb_difference" \
            "unexpected hugetlb charge"
        fi

        if [[ "$reserve" != "-n" ]] || [[ "$populate" == "-o" ]]; then
          check_equal "$((5 * $MB * 1024 * 1024))" "$reserved_difference" \
            "reservation charge mismatch"
        else
          check_equal "0" "$reserved_difference" \
            "unexpected reservation charge"
        fi

        tname=$(test_name "normal" "$method" "$private" "$populate" "$reserve")
        if [[ "$test_failed" -eq 0 ]]; then
          ktap_test_pass "$tname"
        else
          ktap_test_fail "$tname"
        fi

        # --- Test normal case with write ---
        cleanup
        test_failed=0
        run_test 5 "$populate" '-w' 5 5 10 "$method" "$private" "0" "$reserve"

        check_equal "$((5 * $MB * 1024 * 1024))" "$hugetlb_difference" \
          "hugetlb charge mismatch"

        check_equal "$((5 * $MB * 1024 * 1024))" "$reserved_difference" \
          "reservation charge mismatch"

        tname=$(test_name "normal-write" "$method" "$private" "$populate" "$reserve")
        if [[ "$test_failed" -eq 0 ]]; then
          ktap_test_pass "$tname"
        else
          ktap_test_fail "$tname"
        fi

      done # reserve
    done   # private
  done     # method
done       # populate

# Dynamic test count — print trailing plan.
KSFT_NUM_TESTS=$((KTAP_CNT_PASS + KTAP_CNT_FAIL + KTAP_CNT_SKIP))
echo "1..$KSFT_NUM_TESTS"
ktap_finished
