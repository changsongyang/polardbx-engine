#!/bin/bash

readonly DAILY_REGRESSION=1
readonly MERGE_CHECK=2

GET_TEST_TYPE() {
  local res=0
  if [ "$1" = "DAILY_REGRESSION" ]; then
    res=$DAILY_REGRESSION
  elif [ "$1" = "MERGE_CHECK" ]; then
    res=$MERGE_CHECK
  else
    #use daily regression by default
    res=$DAILY_REGRESSION
  fi
  echo "$res"
}

export TEST_TYPE_ENUM=0
TEST_TYPE_ENUM=$(GET_TEST_TYPE "${TEST_TYPE}")

echo "TEST_TYPE_ENUM: $TEST_TYPE_ENUM"
