#!/bin/bash

[ -f "VERSION" ] && OLD_VERSION=$(cat VERSION)

if [ -n "$1" ]
then
    VERSION="$1"
elif [ -d '.git' ]
then
    git update-index --assume-unchanged VERSION
    git update-index --refresh > /dev/null 2>&1
    VERSION=$(git describe --match='[0-9]*' --dirty 2> /dev/null)
fi

[ -n "$VERSION" ] && echo -n "$VERSION" > VERSION

if [ -f "VERSION" ]
then
    VERSION=$(cat VERSION)
else
    VERSION="UNKNOWN"
fi

if [ "$VERSION" != "$OLD_VERSION" ] || [ ! -f "src/version.h" ]
then
    cat <<EOF > src/version.h
#ifndef _VERSION_H_
#define AHOVIEWER_VERSION "$VERSION"
extern const char *const ahoviewer_version;
#endif // _VERSION_H_
EOF
fi

echo $VERSION
