jVal=40
debug_mode=0
release_mode=0
clear_mode=0

while getopts "j:drc" opt; do
  case $opt in
    j) jVal=$OPTARG ;;
    d) debug_mode=1 ;;
    r) release_mode=1 ;;
    c) clear_mode=1 ;;
    *) exit 1 ;;
  esac
done

if [ $clear_mode -eq 1 ]; then
    echo "clear build"
    rm -rf build/*
    exit 0
fi

cd build || exit 1

if [ $debug_mode -eq 1 ]; then
    echo "compile with -DCMAKE_BUILD_TYPE=Debug"
    cmake -DCMAKE_BUILD_TYPE=Debug ..
elif [ $release_mode -eq 1 ]; then
    echo "compile with -DCMAKE_BUILD_TYPE=Release"
    cmake -DCMAKE_BUILD_TYPE=Release ..
else
    echo "compile with -DCMAKE_CXX_FLAGS=-O3"
    cmake -DCMAKE_CXX_FLAGS="-O3" ..
fi

make -j $jVal
