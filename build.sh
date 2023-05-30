#!/bin/bash

# Script for Dev's daily work.  It is a good idea to use the exact same
# build options as the released version.

function version_ge()
{
  if test "$(echo "$@" | tr " " "\n" | sort -rV | head -n 1)" == "$1"
  then
      return 0
  else
      return 1
  fi
}

get_mach_type()
{
  # ARM64: aarch64
  # SW64 : sw_64
  # X86  : x86_64
  mach_type=`uname -m`;
}

get_os_type()
{
  # Windows: MINGW32_NT
  # Mac OSX: Darwin
  # Linux  : Linux
  os_type=`uname`;
  if [ "$(expr substr $(uname -s) 1 10)" == "MINGW32_NT" ]; then
      # Windows NT
      os_type="WIN"
  fi
}

get_linux_version()
{
  if uname -r | grep -q -o "el6\|alios6"
  then
    linux_version="alios6"
  elif uname -r | grep -q -o "el7\|alios7"
  then
    linux_version="alios7"
  else
    linux_version="not_alios"
  fi
}

get_key_value()
{
  echo "$1" | sed 's/^--[a-zA-Z_-]*=//'
}

usage()
{
cat <<EOF
Usage: $0 [-t debug|release] [-d <dest_dir>] [-s <server_suffix>] [-g asan|tsan|ubsan|valg] [-i none|master|raft] [-r]
       Or
       $0 [-h | --help]
  -t                      Select the build type.
  -d                      Set the destination directory.
  -l                      Enable lizard debug mode.
  -s                      Set the server suffix.
  -g                      Enable the sanitizer of compiler, asan for AddressSanitizer, tsan for ThreadSanitizer, ubsan for UndefinedBehaviorSanitizer, valg for valgrind
  -c                      Enable GCC coverage compiler option
  -i                      initialize mysql server
  -r                      rebuild without make chean
  -h, --help              Show this help message.

Note: this script is intended for internal use by MySQL developers.
EOF
}

parse_options()
{
  while test $# -gt 0
  do
    case "$1" in
    -t=*)
      build_type=`get_key_value "$1"`;;
    -t)
      shift
      build_type=`get_key_value "$1"`;;
    -d=*)
      dest_dir=`get_key_value "$1"`;;
    -d)
      shift
      dest_dir=`get_key_value "$1"`;;
    -s=*)
      server_suffix=`get_key_value "$1"`;;
    -s)
      shift
      server_suffix=`get_key_value "$1"`;;
    -g=*)
      san_type=`get_key_value "$1"`;;
    -g)
      shift
      san_type=`get_key_value "$1"`;;
    -c=*)
      enable_gcov=`get_key_value "$1"`;;
    -c)
      shift
      enable_gcov=`get_key_value "$1"`;;
    -l)
      enable_lizard_dbg=1;;
    -i=*)
      initialize_type=`get_key_value "$1"`;;
    -i)
      shift
      initialize_type=`get_key_value "$1"`;;
    -r)
      with_rebuild=1;;
    -h | --help)
      usage
      exit 0;;
    *)
      echo "Unknown option '$1'"
      exit 1;;
    esac
    shift
  done

}

dump_options()
{
  echo "Dumping the options used by $0 ..."
  echo "build_type=$build_type"
  echo "enable_lizard_dbg=$enable_lizard_dbg"
  echo "dest_dir=$dest_dir"
  echo "server_suffix=$server_suffix"
  echo "Sanitizer=$san_type"
  echo "GCOV=$enable_gcov"
  echo "mach_tpye=$mach_type"
  echo "os_type=$os_type"
  echo "cmake_version=$cmake_version"
  echo "cmake_path=$CMAKE"
  echo "gcc_version=$gcc_version"
  echo "gcc_path=$CC"
  echo "cxx_path=$CXX"
  echo "CFLAGS=$CFLAGS"
  echo "CXXFLAGS=$CXXFLAGS"
}

initialize()
{
  echo "
  [mysqld]
  port = 3306
  basedir = $dest_dir
  datadir = $dest_dir/data
  socket = $dest_dir/mysql.sock
  log_error_verbosity=3
  log_error=$dest_dir/mysql-err.log

  sql_mode=STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION
  default_authentication_plugin = 'mysql_native_password'

  #gtid:
  gtid_mode = on
  enforce_gtid_consistency = on

  #binlog
  log_bin = mysql-binlog
  log_slave_updates = on
  binlog_format = row
  binlog-ignore-db=sys
  binlog-ignore-db=information_schema
  binlog-ignore-db=performance_schema
  binlog-do-db=test

  server_id = 1
  " > $HOME/my.cnf

  cat $HOME/my.cnf > $HOME/my2.cnf
  echo "
  port = 3307
  datadir = $dest_dir/data2
  socket = $dest_dir/mysql2.sock
  log_error=$dest_dir/mysql-err2.log
  " >> $HOME/my2.cnf

  cat $HOME/my.cnf > $HOME/my3.cnf
  echo "
  port = 3308
  datadir = $dest_dir/data3
  socket = $dest_dir/mysql3.sock
  log_error=$dest_dir/mysql-err3.log
  " >> $HOME/my3.cnf

  if [ x$initialize_type == x"master" ]; then
    rm -rf $dest_dir/data
    mkdir -p $dest_dir/data
    ./runtime_output_directory/mysqld --defaults-file=$HOME/my.cnf --initialize --cluster-id=1 --cluster-start-index=1 --cluster-info='127.0.0.1:23451@1'
    nohup ./runtime_output_directory/mysqld --defaults-file=$HOME/my.cnf &
  else
    rm -rf $dest_dir/data $dest_dir/data2 $dest_dir/data3
    mkdir -p $dest_dir/data  $dest_dir/data2  $dest_dir/data3
    ./runtime_output_directory/mysqld --defaults-file=$HOME/my.cnf --initialize --cluster-id=1 --cluster-start-index=1 --cluster-info='127.0.0.1:23451;127.0.0.1:23452;127.0.0.1:23453@1'
    ./runtime_output_directory/mysqld --defaults-file=$HOME/my2.cnf --initialize --cluster-id=1 --cluster-start-index=1 --cluster-info='127.0.0.1:23451;127.0.0.1:23452;127.0.0.1:23453@2'
    ./runtime_output_directory/mysqld --defaults-file=$HOME/my3.cnf --initialize --cluster-id=1 --cluster-start-index=1 --cluster-info='127.0.0.1:23451;127.0.0.1:23452;127.0.0.1:23453@3'
    nohup ./runtime_output_directory/mysqld --defaults-file=$HOME/my.cnf &
    nohup ./runtime_output_directory/mysqld --defaults-file=$HOME/my2.cnf &
    nohup ./runtime_output_directory/mysqld --defaults-file=$HOME/my3.cnf &
  fi
}

if test ! -f sql/mysqld.cc
then
  echo "You must run this script from the MySQL top-level directory"
  exit 1
fi

build_type="release"
dest_dir=$HOME/tmp_run
server_suffix="galaxy-dev"
san_type=""
asan=0
tsan=0
ubsan=0
valg=0
gcov=0
enable_gcov=0
enable_lizard_dbg=0
initialize_type="none"
with_rebuild=0

parse_options "$@"

get_mach_type
get_os_type
get_linux_version
commit_id=`git rev-parse --short HEAD`

if [ x"$build_type" = x"debug" ]; then
  build_type="Debug"
  debug=1
  if [ $enable_gcov -eq 1 ]; then
    gcov=1
  else
    gcov=0
  fi
elif [ x"$build_type" = x"release" ]; then
  # Release CMAKE_BUILD_TYPE is not compatible with mysql 8.0
  # build_type="Release"
  build_type="RelWithDebInfo"
  debug=0
  gcov=0
else
  echo "Invalid build type, it must be \"debug\" or \"release\"."
  exit 1
fi

server_suffix="-""$server_suffix"

if [ x"$build_type" = x"RelWithDebInfo" ]; then
  COMMON_FLAGS="-O3 -g -fexceptions -fno-strict-aliasing"
elif [ x"$build_type" = x"Debug" ]; then
  COMMON_FLAGS="-O0 -g3 -gdwarf-2 -fexceptions -fno-strict-aliasing"
fi

if [ x"$mach_type" = x"x86_64" ]; then # X86
  COMMON_FLAGS="$COMMON_FLAGS -fno-omit-frame-pointer -D_GLIBCXX_USE_CXX11_ABI=0"
elif [ x"$mach_type" = x"aarch64" ]; then # ARM64
  # ARM64 needn't more flags
  COMMON_FLAGS="$COMMON_FLAGS" #"-static-libstdc++ -static-libgcc"
fi

COMMON_FLAGS="$COMMON_FLAGS -fdiagnostics-color=always"
export GCC_COLORS='error=01;31:warning=01;35:note=01;36:caret=01;32:locus=01:quote=01'

CFLAGS="$COMMON_FLAGS"
CXXFLAGS="$COMMON_FLAGS"

if [ x"$san_type" = x"" ]; then
  asan=0
  tsan=0
  ubsan=0
  valg=0
elif [ x"$san_type" = x"asan" ]; then
    asan=1
    ## gcov is conflicting with gcc sanitizer (at least for devtoolset-7),
    ## disable gcov if sanitizer is requested
    gcov=0
elif [ x"$san_type" = x"tsan" ]; then
    tsan=1
    ## gcov is conflicting with gcc sanitizer (at least for devtoolset-7),
    ## disable gcov if sanitizer is requested
    gcov=0
elif [ x"$san_type" = x"ubsan" ]; then
    ubsan=1
    ## gcov is conflicting with gcc sanitizer (at least for devtoolset-7),
    ## disable gcov if sanitizer is requested
    gcov=0
elif [ x"$san_type" = x"valg" ]; then
    valg=1
    ## gcov is conflicting with gcc sanitizer (at least for devtoolset-7),
    ## disable gcov if sanitizer is requested
    gcov=0
else
  echo "Invalid sanitizer type, it must be \"asan\" or \"tsan\" or \"ubsan\" or \"valg\"."
  exit 1
fi

CC=gcc
CXX=g++

# Update choosed version
gcc_version=`$CC --version | awk 'NR==1 {print $3}'`
cmake_version=`cmake --version | awk 'NR==1 {print $3}'`

# Dumpl options
dump_options

export CC CFLAGS CXX CXXFLAGS

if [ x"$with_rebuild" = x"1" ]; then
  echo "need rebuild without clean"
else
  echo "need rebuild with clean"
  # Avoid unexpected cmake rerunning
  rm -rf packaging/deb-in/CMakeFiles/progress.marks
  rm -rf CMakeCache.txt
  make clean
  cat extra/boost/boost_1_77_0.tar.bz2.*  > extra/boost/boost_1_77_0.tar.bz2
  cmake .                               \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DFORCE_INSOURCE_BUILD=1           \
      -DCMAKE_BUILD_TYPE="$build_type"   \
      -DWITH_PROTOBUF:STRING=bundled     \
      -DSYSCONFDIR="$dest_dir"           \
      -DCMAKE_INSTALL_PREFIX="$dest_dir" \
      -DMYSQL_DATADIR="$dest_dir/data"   \
      -DMYSQL_UNIX_ADDR="$dest_dir/mysql.sock"   \
      -DWITH_DEBUG=$debug                \
      -DENABLE_GCOV=$gcov                \
      -DINSTALL_LAYOUT=STANDALONE        \
      -DMYSQL_MAINTAINER_MODE=0          \
      -DWITH_EMBEDDED_SERVER=0           \
      -DWITH_SSL=system                  \
      -DWITH_ZLIB=bundled                \
      -DWITH_ZSTD=bundled                \
      -DWITH_MYISAM_STORAGE_ENGINE=1     \
      -DWITH_INNOBASE_STORAGE_ENGINE=1   \
      -DWITH_CSV_STORAGE_ENGINE=1        \
      -DWITH_ARCHIVE_STORAGE_ENGINE=1    \
      -DWITH_BLACKHOLE_STORAGE_ENGINE=1  \
      -DWITH_FEDERATED_STORAGE_ENGINE=1  \
      -DWITH_PERFSCHEMA_STORAGE_ENGINE=1 \
      -DWITH_EXAMPLE_STORAGE_ENGINE=0    \
      -DWITH_TEMPTABLE_STORAGE_ENGINE=1  \
      -DWITH_EXTRA_CHARSETS=all          \
      -DDEFAULT_CHARSET=utf8mb4          \
      -DDEFAULT_COLLATION=utf8mb4_0900_ai_ci \
      -DENABLED_PROFILING=1              \
      -DENABLED_LOCAL_INFILE=1           \
      -DWITH_ASAN=$asan                  \
      -DWITH_TSAN=$tsan                  \
      -DWITH_UBSAN=$ubsan                \
      -DWITH_VALGRIND=$valg              \
      -DENABLE_GPROF=0                   \
      -DWITH_BOOST="./extra/boost/boost_1_77_0.tar.bz2" \
      -DRDS_COMMIT_ID=$commit_id         \
      -DDOWNLOAD_BOOST=0                \
      -DMYSQL_SERVER_SUFFIX="$server_suffix"         \
      -DWITH_UNIT_TESTS=0
fi

make -j `getconf _NPROCESSORS_ONLN`

if [ x$initialize_type != x"none" ]; then
  make -j `getconf _NPROCESSORS_ONLN` install
  initialize
  echo "use follow cmd to login mysql:"
  echo "./runtime_output_directory/mysql -uroot -hlocalhost -P3306 -p"
  echo ""
  echo "use follow sql to modify password:"
  echo "alter user 'root'@'localhost' identified WITH mysql_native_password by '';"
fi

# end of file
