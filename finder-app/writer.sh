#!/bin/bash

# Check if the correct number of arguments is provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <writefile> <writestr>"
    exit 1
fi

# Assign arguments to variables
writefile=$1
writestr=$2

# Extract the directory path from the provided file path
dir=$(dirname "$writefile")

# Create the directory if it doesn't exist
if ! mkdir -p "$dir"; then
  echo "Failed to create directory: $dir"
  exit 1
fi

# Write the string to the file, overwriting if it exists
if ! echo "$writestr" > "$writefile"; then
  echo "Failed to create or write to the file: $writefile"
  exit 1
fi

echo "Successfully wrote to the file: $writefile"
