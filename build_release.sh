#!/bin/bash

# Check if version is provided
if [ -z "$1" ]; then
    echo "Usage: ./build_release.sh <version>"
    echo "Example: ./build_release.sh 1.0.0"
    exit 1
fi

VERSION=$1

# Run the Makefile release target
make release VERSION=$VERSION

echo ""
echo "✅ Binaries compiled successfully!"
echo "Listing contents of releases/v$VERSION:"
ls -lh releases/v$VERSION
