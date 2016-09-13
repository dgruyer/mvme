#!/bin/bash

set -e

if [ $# != 4 ] ; then
    echo "Usage: $0 PATH_TO_BINARY PATH_TO_SOURCES QT_INSTALL_PREFIX TARGET_FOLDER"
    exit 1
fi

PATH_TO_BINARY="$1"
PATH_TO_SOURCES="$2"
QT_INSTALL_PREFIX="$3"
TARGET_FOLDER="$4"

# if we cannot find the the binary we have to abort
if [ ! -f "$PATH_TO_BINARY" ] ; then
    echo "The file '$PATH_TO_BINARY' was not found. Aborting!"
    exit 1
fi

if [ ! -d "$PATH_TO_SOURCES" ] ; then
    echo "The directory '$PATH_TO_SOURCES' was not found. Aborting!"
    exit 1
fi

if [ ! -d "$QT_INSTALL_PREFIX" ] ; then
    echo "The directory '$QT_INSTALL_PREFIX' was not found. Aborting!"
    exit 1
fi

mkdir -p "$TARGET_FOLDER"

# copy the binary to the target folder
# create directories if required
echo "---> copy binary to target folder"
cp -v "$PATH_TO_BINARY" "$TARGET_FOLDER"

# copy the required shared libs to the target folder
# create directories if required
echo "---> copy libraries"
for lib in `ldd "$PATH_TO_BINARY" | cut -d'>' -f2 | awk '{print $1}'` ; do
   if [ -f "$lib" ] ; then
        cp -v "$lib" "$TARGET_FOLDER"
   fi  
done

echo "---> copy additional qt libraries"
cp -v "$QT_INSTALL_PREFIX/lib/libQt5DBus.so.5" "$TARGET_FOLDER"

echo "---> copy qt plugins"
cp -vr "$QT_INSTALL_PREFIX/plugins/platforms" "$TARGET_FOLDER"
cp -vr "$QT_INSTALL_PREFIX/plugins/imageformats" "$TARGET_FOLDER"

echo "---> copy auxiliary files"
cp -v "$PATH_TO_SOURCES/README.txt" "$TARGET_FOLDER"
cp -v "$PATH_TO_SOURCES/mvme2.sh" "$TARGET_FOLDER"
cp -v "$PATH_TO_SOURCES/qt.conf" "$TARGET_FOLDER"
cp -vr "$PATH_TO_SOURCES/templates" "$TARGET_FOLDER"
