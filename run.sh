# set -x

SRC_ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

cd $SRC_ROOT_DIR

PROJECT_PATH="$SRC_ROOT_DIR"

BUILD_PATH="$SRC_ROOT_DIR/.build"
[[ ! -z "${getDevBuildPath}" ]] && eval "$getDevBuildPath" && BUILD_PATH=$(getDevBuildPath);

NUM_CPU=$(getconf _NPROCESSORS_ONLN)
# NUM_CPU=1

type "cmake" > /dev/null 2>&1;
cmake_available=$?
set -e


if [[ "$cmake_available" == "0" ]]; then
  MAKE_ARGS="-s"
  # VERBOSE_ARGS="-DCMAKE_VERBOSE_MAKEFILE=TRUE"
fi

if [[ $1 == "-v" ]]; then
  shift;
  if [[ "$cmake_available" == "0" ]]; then
    MAKE_ARGS=""
  else
    MAKE_ARGS="-v"
  fi
fi

run(){
  cd $BUILD_PATH
  ./$@
  cd ~-
}

printSize(){
  echo BUILD_PATH: $BUILD_PATH
  cd $BUILD_PATH

  exec_files_array=(${@})
  if [ -z "$exec_files_array" ]; then
    executables=$(find . -maxdepth 1 -executable -type f -printf '%f ' || find . -maxdepth 1 -perm +0111 -type f)
    if [ ! -z "$executables" ]; then
      echo $executables | xargs md5
      echo $executables | xargs size
      echo $executables | xargs ls -la
    fi
    return
  fi

  for exec in "${exec_files_array[@]}"
  do
    md5 $exec
    size $exec
    ls -la $exec
  done
  cd ~-
}

build_inline(){
  echo "cmake not found, building inline without airplay"
  mkdir -p $BUILD_PATH
  APP=$BUILD_PATH/$1
  clang $MAKE_ARGS -DNO_AIRPLAY -Wl,-dead_strip pip/*.m -o $APP -fobjc-arc -fobjc-link-runtime -O3 -g0 -Wno-deprecated-declarations -F /System/Library/PrivateFrameworks \
    -Wno-availability -Wno-nullability-completeness -mmacosx-version-min=11.0 -Ipip/priv_headers -F /System/Volumes/Data/Library/Developer/CommandLineTools/SDKs/MacOSX12.3.sdk/System/Library/Frameworks -framework ScreenCaptureKit -framework AVFoundation \
    -framework Cocoa -framework VideoToolbox -framework AudioToolbox -framework CoreMedia -framework QuartzCore -framework OpenGL -framework Metal -framework MetalKit -framework PIP -framework SkyLight
  printSize $1;
}

build(){
  mkdir -p $BUILD_PATH
  cd $BUILD_PATH
  [[ ! -f "Makefile" ]] && cmake $VERBOSE_ARGS $PROJECT_PATH
  make $MAKE_ARGS -j $NUM_CPU $@
  printSize $@;
  cd ~-
}

debug(){
  cd $BUILD_PATH
  lldb $@
  # | TZ=UTC ts '[%F %.T]'
  cd ~-
}

build_app(){
  if [[ "$cmake_available" == "0" ]]; then
    build $@;
  else
    build_inline $@
  fi
}

ACTION="$1"

[[ -z "$ACTION" ]] || shift;

if [[ $ACTION == "b" ]]; then
  app=$1;shift;
  build_app $app;
elif [[ $ACTION == "br" ]]; then
  app=$1;shift;
  build_app $app;
  run $app "$@";
elif [[ $ACTION == "bd" ]]; then
  app=$1;shift;
  build_app $app;
  debug $app $@;
else
  build_app pip
  run pip $@;
fi
